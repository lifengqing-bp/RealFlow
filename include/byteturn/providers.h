#pragma once

#include "byteturn/types.h"

#include <functional>
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

}  // namespace byteturn
