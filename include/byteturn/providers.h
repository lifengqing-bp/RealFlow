#pragma once

#include "byteturn/types.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace byteturn {

class AsrProvider {
 public:
  virtual ~AsrProvider() = default;
  virtual void reset() = 0;
  virtual void push(const AudioFrame& frame,
                    const std::function<void(std::string, bool)>& on_transcript) = 0;
};

class LlmProvider {
 public:
  using TextDeltaSink = std::function<bool(const std::string&)>;
  virtual ~LlmProvider() = default;
  virtual LlmTurn complete(const std::vector<Message>& history) = 0;
  virtual LlmTurn complete(const std::vector<Message>& history,
                           const std::function<bool()>& cancelled) {
    (void)cancelled;
    return complete(history);
  }
  virtual LlmTurn stream(const std::vector<Message>& history,
                         const TextDeltaSink& on_text_delta,
                         const std::function<bool()>& cancelled) {
    LlmTurn turn = complete(history, cancelled);
    if (!turn.text.empty() && !on_text_delta(turn.text))
      throw std::runtime_error("LLM stream consumer stopped");
    return turn;
  }
};

class TtsProvider {
 public:
  virtual ~TtsProvider() = default;
  virtual void synthesize(const std::string& text,
                          const std::function<bool(const AudioFrame&)>& on_audio) = 0;
  virtual void begin_utterance() {}
  virtual void synthesize_chunk(
      const std::string& text,
      const std::function<bool(const AudioFrame&)>& on_audio) {
    synthesize(text, on_audio);
  }
  virtual void end_utterance() {}
  virtual void cancel() = 0;
};

// Events emitted by a native, bidirectional speech-to-speech provider. Provider
// callbacks may arrive on an I/O thread and must never be blocked by consumers.
enum class RealtimeEventType {
  Connected,
  InputSpeechStarted,
  InputSpeechEnded,
  TranscriptPartial,
  TranscriptFinal,
  ResponseStarted,
  AudioDelta,
  ResponseCompleted,
  Error,
  Closed
};

struct RealtimeEvent {
  RealtimeEventType type = RealtimeEventType::Connected;
  std::string response_id;
  std::string text;
  AudioFrame audio;
  std::string error_code;
};

struct RealtimeSessionConfig {
  std::string session_id;
  int input_sample_rate_hz = 16000;
  int output_sample_rate_hz = 24000;
  bool server_vad = true;
};

class RealtimeSpeechSession {
 public:
  virtual ~RealtimeSpeechSession() = default;

  // All methods are thread-safe. push_audio must be non-blocking or perform
  // only a bounded copy because it is called from ByteTurn's audio worker.
  virtual bool push_audio(const AudioFrame& frame) = 0;
  virtual void commit_input() = 0;

  // Stop generation and truncate the provider's conversation item to audio
  // that the user actually heard, not to audio merely received from the wire.
  virtual void cancel_response(const std::string& response_id,
                               std::uint64_t played_samples,
                               int sample_rate_hz) = 0;
  // close() must stop delivery of callbacks before it returns.
  virtual void close() = 0;
};

class RealtimeSpeechProvider {
 public:
  using EventSink = std::function<void(RealtimeEvent)>;
  virtual ~RealtimeSpeechProvider() = default;
  virtual std::unique_ptr<RealtimeSpeechSession> connect(
      const RealtimeSessionConfig& config, EventSink sink) = 0;
};

}  // namespace byteturn
