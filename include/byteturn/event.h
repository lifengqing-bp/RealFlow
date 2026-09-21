#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>
#include <utility>

namespace byteturn {

enum class EventType {
  RealtimeSessionStarted,
  InputSpeechStarted,
  InputSpeechEnded,
  AsrEndOfUtterance,
  TranscriptPartial,
  TranscriptFinal,
  TurnStarted,
  ModelStarted,
  FirstToken,
  ModelTextDelta,
  ModelCompleted,
  ToolStarted,
  ToolCompleted,
  SpeechStarted,
  TtsChunkStarted,
  FirstAudio,
  AudioOutput,
  PlaybackStarted,
  PlaybackProgress,
  BargeInDetected,
  OutputCancelled,
  SpeechCompleted,
  ConversationTurnCompleted,
  TurnCompleted,
  TurnCancelled,
  RealtimeSessionClosed,
  Error,
  // Generic session lifetime, distinct from a provider connection lifetime.
  SessionStarted,
  SessionStopped,
  SessionFailed
};

struct Event {
  Event(EventType event_type, std::string session = {}, std::string turn = {},
        std::uint64_t event_sequence = 0,
        std::chrono::steady_clock::time_point event_timestamp = {},
        std::string event_name = {}, std::string event_data = {},
        std::string trace = {})
      : type(event_type), session_id(std::move(session)), turn_id(std::move(turn)),
        sequence(event_sequence), timestamp(event_timestamp),
        name(std::move(event_name)), data(std::move(event_data)),
        trace_id(std::move(trace)) {}

  EventType type;
  std::string session_id;
  std::string turn_id;
  std::uint64_t sequence = 0;
  std::chrono::steady_clock::time_point timestamp =
      std::chrono::steady_clock::now();
  std::string name;
  std::string data;
  std::string trace_id;

  // timestamp retains a supplied local/source time for legacy metrics.
  // received_at and sequence are assigned by the admitting timeline, once.
  std::chrono::steady_clock::time_point received_at{};
  std::uint64_t generation = 0;  // 0 means unspecified at legacy ingress.
};

class EventBus {
 public:
  using Handler = std::function<void(const Event&)>;
  using Subscription = std::uint64_t;

  Subscription subscribe(Handler handler);
  void unsubscribe(Subscription subscription);
  void publish(Event event);

  // True inside any EventBus subscriber on this thread, including nested
  // publication. Blocking lifecycle waits must not run in these callbacks.
  static bool in_callback() noexcept;

 private:
  friend class EventTimeline;
  // Only canonical timeline delivery may bypass legacy normalization.
  std::size_t deliver(const Event& event);

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
