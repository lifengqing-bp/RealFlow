#pragma once

#include "byteturn/providers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace byteturn {

// Local development protocol, not a vendor API. Connects only to 127.0.0.1.
// Each call has one deadline covering connect, handshake, send and all replies.
struct WebSocketMockConfig {
  std::uint16_t port = 8765;
  std::chrono::milliseconds timeout{2000};
  std::size_t max_message_bytes = 64 * 1024;
  std::size_t max_response_bytes = 1024 * 1024;
  std::size_t max_response_messages = 256;
};

// Call push serially. reset is a non-waiting cancellation/connection-reset
// request and may race push; the next push reconnects. A callback already in
// progress may finish. The caller must drain push before destroying the adapter.
class WebSocketMockAsr final : public AsrProvider {
 public:
  explicit WebSocketMockAsr(WebSocketMockConfig config = {});
  ~WebSocketMockAsr() override;
  void reset() override;
  void push(const AudioFrame& frame,
            const std::function<void(std::string, bool)>& on_transcript) override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Each invocation owns a fresh connection. No retries or retained history.
// Consumer rejection, cancellation, disconnect, deadline and protocol failures
// throw; a missing terminal done message never becomes a successful response.
class WebSocketMockLlm final : public LlmProvider {
 public:
  explicit WebSocketMockLlm(WebSocketMockConfig config = {});
  LlmTurn complete(const std::vector<Message>& history) override;
  LlmTurn complete(const std::vector<Message>& history,
                   const std::function<bool()>& cancelled) override;
  LlmTurn stream(const std::vector<Message>& history, const TextDeltaSink& sink,
                 const std::function<bool()>& cancelled) override;
 private:
  const WebSocketMockConfig config_;
};

// Serialize begin/synthesize/end; only cancel may run concurrently. Cancellation
// aborts socket I/O cooperatively and does not wait for a server acknowledgement.
// A blocked application callback cannot be interrupted by this adapter.
class WebSocketMockTts final : public TtsProvider {
 public:
  explicit WebSocketMockTts(WebSocketMockConfig config = {});
  void begin_utterance() override;
  void cancel() override;
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>& on_audio) override;
  void synthesize_chunk(const std::string& text,
                        const std::function<bool(const AudioFrame&)>& on_audio) override;
 private:
  const WebSocketMockConfig config_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace byteturn
