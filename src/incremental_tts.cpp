#include "byteturn/incremental_tts.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

IncrementalTtsPipeline::IncrementalTtsPipeline(
    TtsProvider& tts, AudioSink audio_sink, Cancelled cancelled,
    std::size_t max_queued_chunks)
    : tts_(tts), audio_sink_(std::move(audio_sink)),
      cancelled_(std::move(cancelled)), max_queued_chunks_(max_queued_chunks) {
  if (max_queued_chunks_ == 0)
    throw std::invalid_argument("TTS queue capacity must be positive");
  worker_ = std::thread([this] { worker_loop(); });
}

IncrementalTtsPipeline::~IncrementalTtsPipeline() {
  cancel();
  join();
}

bool IncrementalTtsPipeline::push(std::string text) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] {
    return finishing_ || cancelled_locally_ || error_ ||
           queue_.size() < max_queued_chunks_;
  });
  if (finishing_ || cancelled_locally_ || error_ || (cancelled_ && cancelled_()))
    return false;
  queue_.push_back(std::move(text));
  cv_.notify_all();
  return true;
}

void IncrementalTtsPipeline::finish() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finishing_ = true;
  }
  cv_.notify_all();
  join();
  if (error_) std::rethrow_exception(error_);
}

void IncrementalTtsPipeline::cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_locally_ || joined_) return;
    cancelled_locally_ = true;
    queue_.clear();
  }
  tts_.cancel();
  cv_.notify_all();
}

void IncrementalTtsPipeline::join() {
  if (!joined_ && worker_.joinable()) worker_.join();
  joined_ = true;
}

void IncrementalTtsPipeline::worker_loop() {
  try {
    tts_.begin_utterance();
    while (true) {
      std::string chunk;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
          return cancelled_locally_ || !queue_.empty() || finishing_;
        });
        if (cancelled_locally_ || (cancelled_ && cancelled_()))
          break;
        if (queue_.empty() && finishing_) break;
        if (queue_.empty()) continue;
        chunk = std::move(queue_.front());
        queue_.pop_front();
        cv_.notify_all();
      }
      tts_.synthesize_chunk(chunk, [this](const AudioFrame& frame) {
        if (cancelled_locally_ || (cancelled_ && cancelled_())) return false;
        return audio_sink_(frame);
      });
      if (cancelled_ && cancelled_()) break;
    }
    if (!cancelled_locally_ && !(cancelled_ && cancelled_())) tts_.end_utterance();
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::current_exception();
    cancelled_locally_ = true;
    queue_.clear();
    cv_.notify_all();
  }
}

}  // namespace byteturn
