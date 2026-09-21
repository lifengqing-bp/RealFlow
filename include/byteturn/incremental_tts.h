#pragma once

#include "byteturn/providers.h"

#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace byteturn {

class IncrementalTtsPipeline {
 public:
  using AudioSink = std::function<bool(const AudioFrame&)>;
  using Cancelled = std::function<bool()>;

  IncrementalTtsPipeline(TtsProvider& tts, AudioSink audio_sink,
                         Cancelled cancelled = {}, std::size_t max_queued_chunks = 4);
  ~IncrementalTtsPipeline();
  IncrementalTtsPipeline(const IncrementalTtsPipeline&) = delete;
  IncrementalTtsPipeline& operator=(const IncrementalTtsPipeline&) = delete;

  bool push(std::string text);
  void finish();
  void cancel();

 private:
  void worker_loop();
  void join();

  TtsProvider& tts_;
  AudioSink audio_sink_;
  Cancelled cancelled_;
  const std::size_t max_queued_chunks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  std::thread worker_;
  std::exception_ptr error_;
  bool finishing_ = false;
  std::atomic<bool> cancelled_locally_{false};
  bool joined_ = false;
};

}  // namespace byteturn
