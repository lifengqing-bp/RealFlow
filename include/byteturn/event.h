#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>

namespace byteturn {

enum class EventType {
  TranscriptPartial,
  TranscriptFinal,
  TurnStarted,
  ModelStarted,
  ModelCompleted,
  ToolStarted,
  ToolCompleted,
  SpeechStarted,
  AudioOutput,
  TurnCompleted,
  TurnCancelled,
  Error
};

struct Event {
  EventType type;
  std::string session_id;
  std::string turn_id;
  std::uint64_t sequence = 0;
  std::chrono::steady_clock::time_point timestamp =
      std::chrono::steady_clock::now();
  std::string name;
  std::string data;
};

class EventBus {
 public:
  using Handler = std::function<void(const Event&)>;
  using Subscription = std::uint64_t;

  Subscription subscribe(Handler handler);
  void unsubscribe(Subscription subscription);
  void publish(Event event);

 private:
  struct HandlerEntry {
    explicit HandlerEntry(Handler value) : handler(std::move(value)) {}
    Handler handler;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t active_calls = 0;
    bool enabled = true;
  };
  std::mutex mutex_;
  std::unordered_map<Subscription, std::shared_ptr<HandlerEntry>> handlers_;
  Subscription next_subscription_ = 1;
  std::uint64_t next_sequence_ = 1;
};

}  // namespace byteturn
