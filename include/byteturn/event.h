#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

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
  std::mutex mutex_;
  std::unordered_map<Subscription, Handler> handlers_;
  Subscription next_subscription_ = 1;
  std::uint64_t next_sequence_ = 1;
};

}  // namespace byteturn

