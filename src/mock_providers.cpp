#include "byteturn/mock_providers.h"

#include <utility>

namespace byteturn {

MockAsrProvider::MockAsrProvider(std::vector<MockAsrUtterance> script)
    : script_(std::move(script)) {}

void MockAsrProvider::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  utterance_ = partial_ = 0;
}

void MockAsrProvider::push(
    const AudioFrame& frame,
    const std::function<void(std::string, bool)>& on_transcript) {
  std::string text;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (utterance_ == script_.size()) return;
    const auto& item = script_[utterance_];
    if (frame.end_of_utterance) {
      text = item.final_text;
      ++utterance_;
      partial_ = 0;
    } else {
      if (frame.samples.empty() || partial_ == item.partials.size()) return;
      text = item.partials[partial_++];
    }
  }
  on_transcript(std::move(text), frame.end_of_utterance);
}

MockLlmProvider::MockLlmProvider(std::vector<MockLlmReply> script)
    : script_(std::move(script)) {}

LlmTurn MockLlmProvider::complete(const std::vector<Message>& history) {
  return complete(history, {});
}

LlmTurn MockLlmProvider::complete(const std::vector<Message>& history,
                                const std::function<bool()>& cancelled) {
  return stream(history, [](const std::string&) { return true; }, cancelled);
}

LlmTurn MockLlmProvider::stream(const std::vector<Message>&,
                              const TextDeltaSink& sink,
                              const std::function<bool()>& cancelled) {
  const auto check_cancelled = [&] {
    if (cancelled && cancelled()) throw std::runtime_error("mock LLM cancelled");
  };
  check_cancelled();
  const MockLlmReply* reply;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_ == script_.size()) throw std::runtime_error("mock LLM script exhausted");
    reply = &script_[next_++];
  }
  LlmTurn result;
  for (const auto& delta : reply->text_deltas) {
    check_cancelled();
    if (delta.empty()) continue;
    if (!sink(delta)) throw std::runtime_error("mock LLM consumer stopped");
    result.text += delta;
  }
  check_cancelled();
  result.tool_calls = reply->tool_calls;
  return result;
}

MockTtsProvider::MockTtsProvider(std::vector<AudioFrame> frames)
    : frames_(std::move(frames)) {
  for (const auto& frame : frames_) {
    if (frame.sample_rate_hz <= 0 || frame.channels <= 0 ||
        frame.samples.empty() || frame.samples.size() % frame.channels != 0)
      throw std::invalid_argument("invalid mock PCM frame");
  }
}

void MockTtsProvider::begin_utterance() { cancelled_.store(false); }
void MockTtsProvider::cancel() { cancelled_.store(true); }

void MockTtsProvider::synthesize(
    const std::string& text, const std::function<bool(const AudioFrame&)>& on_audio) {
  begin_utterance();
  synthesize_chunk(text, on_audio);
  end_utterance();
}

void MockTtsProvider::synthesize_chunk(
    const std::string& text, const std::function<bool(const AudioFrame&)>& on_audio) {
  if (text.empty()) return;
  for (const auto& frame : frames_) {
    if (cancelled_.load() || !on_audio(frame)) return;
  }
}

}  // namespace byteturn
