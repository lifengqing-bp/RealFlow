#include "byteturn/event_timeline.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <new>
#include <stdexcept>
#include <utility>

namespace byteturn {
namespace {
thread_local const void* active_timeline = nullptr;

bool fits(const Event& event, std::size_t limit) {
  std::size_t total = 0;
  for (const auto* field : {&event.session_id, &event.turn_id, &event.name,
                            &event.data, &event.trace_id}) {
    if (field->size() > limit - total) return false;
    total += field->size();
  }
  return true;
}
}  // namespace

struct EventTimeline::State {
  State(EventBus* external, TimelineConfig options)
      : config(std::move(options)), bus(external ? external : &owned_bus) {}
  TimelineConfig config;
  EventBus owned_bus;
  EventBus* bus;  // Borrowed external bus must outlive shutdown().
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::shared_ptr<const Event>> events;
  std::deque<std::shared_ptr<const Event>> pending;
  TimelineStats counters;
  std::uint64_t enqueued = 0;
  std::uint64_t completed = 0;
  bool closed = false;
};

EventTimeline::EventTimeline(EventBus* bus, TimelineConfig config)
    : state_(std::make_shared<State>(bus, std::move(config))) {
  const auto& c = state_->config;
  if (!c.max_events || !c.max_pending_notifications || !c.max_event_bytes ||
      !c.generation || !c.clock)
    throw std::invalid_argument("invalid timeline capacity, generation or clock");
  dispatcher_ = std::thread([state = state_] { dispatch_loop(state); });
}

EventTimeline::~EventTimeline() { shutdown(); }

TimelineAppendResult EventTimeline::admit(const std::shared_ptr<State>& state,
                                         Event event, Event* normalized) {
  std::lock_guard<std::mutex> lock(state->mutex);
  const auto reject = [&state](TimelineStatus status) {
    ++state->counters.rejected;
    return TimelineAppendResult{status, 0, false};
  };
  if (state->closed) return reject(TimelineStatus::Closed);
  const auto& config = state->config;
  if (!config.session_id.empty()) {
    if (!event.session_id.empty() && event.session_id != config.session_id)
      return reject(TimelineStatus::WrongSession);
    event.session_id = config.session_id;
  }
  if (event.generation && event.generation != config.generation)
    return reject(TimelineStatus::StaleGeneration);
  event.generation = config.generation;
  if (event.trace_id.empty() && !event.session_id.empty() && !event.turn_id.empty())
    event.trace_id = event.session_id + ":" + event.turn_id;
  if (!fits(event, config.max_event_bytes))
    return reject(TimelineStatus::EventTooLarge);

  event.received_at = config.clock();
  if (event.timestamp.time_since_epoch().count() == 0)
    event.timestamp = event.received_at;
  event.sequence = state->counters.accepted + 1;
  // Copy strings by value rather than retaining a producer's potentially huge
  // reserved capacity via move. The bound is on logical string bytes; allocator
  // bookkeeping, shared ownership and snapshot copies have separate overhead.
  auto admitted = std::make_shared<const Event>(event);
  if (normalized) *normalized = *admitted;
  state->events.push_back(admitted);
  ++state->counters.accepted;
  if (state->events.size() > config.max_events) {
    state->events.pop_front();
    ++state->counters.evicted;
  }

  bool enqueued = false;
  if (state->pending.size() < config.max_pending_notifications) {
    try {
      state->pending.push_back(admitted);
      ++state->enqueued;
      state->counters.notification_high_watermark = std::max(
          state->counters.notification_high_watermark, state->pending.size());
      enqueued = true;
      state->cv.notify_all();
    } catch (const std::bad_alloc&) {
      // Journal admission already succeeded. Treat observer allocation failure
      // as an explicit notification gap, not as a failed/ambiguous admission.
      ++state->counters.notification_allocation_failures;
    }
  }
  if (!enqueued) {
    ++state->counters.dropped_notifications;
    if (!state->counters.first_dropped_sequence)
      state->counters.first_dropped_sequence = admitted->sequence;
    state->counters.last_dropped_sequence = admitted->sequence;
  }
  return {TimelineStatus::Accepted, admitted->sequence, enqueued};
}

TimelineAppendResult EventTimeline::append(Event event, Event* normalized) {
  return admit(state_, std::move(event), normalized);
}

EventTimeline::Sink EventTimeline::sink() const {
  std::weak_ptr<State> weak = state_;
  return [weak](Event event) {
    const auto state = weak.lock();
    if (!state) return TimelineAppendResult{};
    return admit(state, std::move(event), nullptr);
  };
}

std::vector<Event> EventTimeline::snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<Event> copy;
  copy.reserve(state_->events.size());
  for (const auto& event : state_->events) copy.push_back(*event);
  return copy;
}

TimelineStats EventTimeline::stats() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto result = state_->counters;
  result.retained = state_->events.size();
  result.pending_notifications = state_->pending.size();
  if (!state_->events.empty()) {
    result.oldest_sequence = state_->events.front()->sequence;
    result.newest_sequence = state_->events.back()->sequence;
  }
  return result;
}

std::size_t EventTimeline::size() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->events.size();
}

EventTimeline::Subscription EventTimeline::subscribe(Handler handler) {
  return state_->bus->subscribe(std::move(handler));
}

void EventTimeline::unsubscribe(Subscription subscription) {
  state_->bus->unsubscribe(subscription);
}

bool EventTimeline::is_dispatch_thread() const noexcept {
  return active_timeline == state_.get();
}

bool EventTimeline::flush(std::chrono::milliseconds timeout) {
  if (is_dispatch_thread() || EventBus::in_callback()) return false;
  std::unique_lock<std::mutex> lock(state_->mutex);
  const auto target = state_->enqueued;
  return state_->cv.wait_for(lock, timeout, [&] {
    return state_->completed >= target;
  });
}

void EventTimeline::close() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->closed = true;
  state_->cv.notify_all();
}

void EventTimeline::shutdown() {
  close();
  if (is_dispatch_thread())
    throw std::logic_error("destroy/join timeline outside its observer callbacks");
  // shutdown and destruction are owner-thread operations, not concurrent joins.
  if (dispatcher_.joinable()) dispatcher_.join();
}

EventBus& EventTimeline::bus() { return *state_->bus; }
const EventBus& EventTimeline::bus() const { return *state_->bus; }

void EventTimeline::dispatch_loop(const std::shared_ptr<State>& state) {
  active_timeline = state.get();
  for (;;) {
    std::shared_ptr<const Event> event;
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      state->cv.wait(lock, [&] { return state->closed || !state->pending.empty(); });
      if (state->pending.empty()) break;
      event = std::move(state->pending.front());
      state->pending.pop_front();
    }
    std::size_t failures = 0;
    try {
      // No timeline lock and no metadata rewrite during subscriber delivery.
      failures = state->bus->deliver(*event);
    } catch (...) {
      // Also contain dispatch infrastructure exceptions (e.g. allocation).
      failures = 1;
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->counters.observer_failures += failures;
      ++state->completed;
      state->cv.notify_all();
    }
  }
  active_timeline = nullptr;
}

}  // namespace byteturn
