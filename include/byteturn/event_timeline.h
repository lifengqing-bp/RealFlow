#pragma once

#include "byteturn/event.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace byteturn {

struct TimelineConfig {
  std::size_t max_events = 1024;
  std::size_t max_pending_notifications = 1024;
  // Aggregate string bytes per event, not PCM. Bounds retained and pending
  // payload memory in addition to bounding the number of event objects.
  std::size_t max_event_bytes = 64 * 1024;
  std::string session_id;  // Empty only for an unscoped standalone timeline.
  std::uint64_t generation = 1;
  // Must be short, thread-safe and non-reentrant. Called at admission.
  std::function<std::chrono::steady_clock::time_point()> clock =
      [] { return std::chrono::steady_clock::now(); };
};

enum class TimelineStatus {
  Accepted,
  Closed,
  WrongSession,
  StaleGeneration,
  EventTooLarge
};

struct TimelineAppendResult {
  TimelineStatus status = TimelineStatus::Closed;
  std::uint64_t sequence = 0;
  bool notification_enqueued = false;
  explicit operator bool() const { return status == TimelineStatus::Accepted; }
};

struct TimelineStats {
  std::uint64_t accepted = 0;
  std::uint64_t evicted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t dropped_notifications = 0;
  std::uint64_t observer_failures = 0;
  std::uint64_t notification_allocation_failures = 0;
  // A span of possible gaps, not a claim that every ID in the span was lost.
  std::uint64_t first_dropped_sequence = 0;
  std::uint64_t last_dropped_sequence = 0;
  std::uint64_t oldest_sequence = 0;
  std::uint64_t newest_sequence = 0;
  std::size_t retained = 0;
  std::size_t pending_notifications = 0;
  std::size_t notification_high_watermark = 0;
};

// One bounded, process-local journal with one ordered observer delivery lane.
// Admission normalizes metadata before storage and never calls subscribers.
// Delivery is asynchronous and best-effort under overload; it is NOT a command
// queue. Inspect stats/gaps and use snapshots when a notification is dropped.
class EventTimeline {
 public:
  using Handler = EventBus::Handler;
  using Subscription = EventBus::Subscription;
  using Sink = std::function<TimelineAppendResult(Event)>;

  explicit EventTimeline(EventBus* bus = nullptr, TimelineConfig config = {});
  ~EventTimeline();
  EventTimeline(const EventTimeline&) = delete;
  EventTimeline& operator=(const EventTimeline&) = delete;

  // normalized, when supplied, receives the exact admitted event. No value is
  // written on rejection. Supplying an old bus sequence does not preserve it.
  TimelineAppendResult append(Event event, Event* normalized = nullptr);
  Sink sink() const;  // Weak lifetime: calls after destruction return Closed.
  std::vector<Event> snapshot() const;
  TimelineStats stats() const;
  std::size_t size() const;

  Subscription subscribe(Handler handler);
  void unsubscribe(Subscription subscription);
  // Wait only for notifications enqueued before this call. False on timeout
  // or callback re-entry. It does not mean no notifications were dropped.
  bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  void close();  // Close admission; drain accepted notifications asynchronously.
  void shutdown();  // Close and join. Must be called from the owning thread.
  bool is_dispatch_thread() const noexcept;

  // Legacy observation bridge only. Direct publish bypasses the journal and
  // is not a supported session ingress. Prefer subscribe/unsubscribe above.
  EventBus& bus();
  const EventBus& bus() const;

 private:
  struct State;
  static TimelineAppendResult admit(const std::shared_ptr<State>& state,
                                   Event event, Event* normalized);
  static void dispatch_loop(const std::shared_ptr<State>& state);
  std::shared_ptr<State> state_;
  std::thread dispatcher_;
};

}  // namespace byteturn
