#pragma once

#include "byteturn/providers.h"

#include <atomic>
#include <mutex>

namespace byteturn {

struct MockAsrUtterance {
  std::vector<std::string> partials;
  std::string final_text;
};

// Scripted, not speech recognition. Each nonempty non-final input frame emits
// the next partial; an end_of_utterance frame emits the final and advances.
// Exhaustion is silent. reset() rewinds the entire script. Calls are serialized
// by the caller except reset(), which may race push(). A callback already
// selected by push may finish after reset; reset is not a callback-drain barrier.
class MockAsrProvider final : public AsrProvider {
 public:
  explicit MockAsrProvider(std::vector<MockAsrUtterance> script = {{ {}, "Hello." }});
  void reset() override;
  void push(const AudioFrame& frame,
            const std::function<void(std::string, bool)>& on_transcript) override;
 private:
  const std::vector<MockAsrUtterance> script_;
  std::mutex mutex_;
  std::size_t utterance_ = 0, partial_ = 0;
};

struct MockLlmReply {
  std::vector<std::string> text_deltas;
  std::vector<ToolCall> tool_calls;
};

// One scripted reply per invocation, independent of history. Exhaustion throws.
// Complete and stream consume the same script. Once selected, a reply is consumed
// even on cancellation, consumer rejection or callback exception. A pre-cancelled
// invocation consumes nothing. Concurrent selection is safe; order is scheduling
// dependent. No callback is retained or invoked under the script mutex.
class MockLlmProvider final : public LlmProvider {
 public:
  explicit MockLlmProvider(std::vector<MockLlmReply> script = {{{"Hello from RealFlow."}, {}}});
  LlmTurn complete(const std::vector<Message>& history) override;
  LlmTurn complete(const std::vector<Message>& history,
                   const std::function<bool()>& cancelled) override;
  LlmTurn stream(const std::vector<Message>& history, const TextDeltaSink& sink,
                 const std::function<bool()>& cancelled) override;
 private:
  const std::vector<MockLlmReply> script_;
  std::mutex mutex_;
  std::size_t next_ = 0;
};

// Replays owned PCM fixtures for each nonempty text chunk; not intelligible
// speech. Defaults to one 10 ms mono silence frame. No pacing, threads or queues.
// Synthesis/begin/end must be serialized; only cancel() may run concurrently.
// Cancellation is cooperative: an already-entered callback may finish.
class MockTtsProvider final : public TtsProvider {
 public:
  explicit MockTtsProvider(std::vector<AudioFrame> frames = {
      {std::vector<std::int16_t>(160, 0), 16000, 1, false}});
  // Standalone synthesis starts a fresh utterance. Incremental users call
  // begin_utterance once, then synthesize_chunk without clearing cancellation.
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>& on_audio) override;
  void begin_utterance() override;
  void synthesize_chunk(const std::string& text,
                        const std::function<bool(const AudioFrame&)>& on_audio) override;
  void cancel() override;
 private:
  const std::vector<AudioFrame> frames_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace byteturn
