#pragma once

#include "byteturn/types.h"

#include <functional>
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
  virtual ~LlmProvider() = default;
  virtual LlmTurn complete(const std::vector<Message>& history) = 0;
};

class TtsProvider {
 public:
  virtual ~TtsProvider() = default;
  virtual void synthesize(const std::string& text,
                          const std::function<bool(const AudioFrame&)>& on_audio) = 0;
  virtual void cancel() = 0;
};

}  // namespace byteturn

