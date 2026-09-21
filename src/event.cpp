#include "byteturn/event.h"

#include <stdexcept>
#include <vector>

namespace byteturn {
namespace {
// Stack frames, rather than a single active pointer, also cover a subscriber
// recursively publishing an event whose subscriber removes its ancestor.
struct ActiveHandler {
  const void* entry;
  ActiveHandler* previous;
};
thread_local ActiveHandler* active_handler = nullptr;

std::size_t calls_on_this_thread(const void* entry) {
  std::size_t count = 0;
  for (auto* frame = active_handler; frame; frame = frame->previous)
    if (frame->entry == entry) ++count;
  return count;
}
}  // namespace

bool EventBus::in_callback() noexcept { return active_handler != nullptr; }

EventBus::Subscription EventBus::subscribe(Handler handler) {
  if (!handler) throw std::invalid_argument("event handler is required");
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = next_subscription_++;
  handlers_.emplace(id, std::make_shared<HandlerEntry>(std::move(handler)));
  return id;
}

void EventBus::unsubscribe(Subscription subscription) {
  std::shared_ptr<HandlerEntry> entry;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = handlers_.find(subscription);
    if (it == handlers_.end()) return;
    entry = it->second;
    handlers_.erase(it);
  }
  std::unique_lock<std::mutex> lock(entry->mutex);
  entry->enabled = false;
  // Never wait on our own callback stack. External removals still quiesce all
  // calls. Subscribers must not mutually wait for removal across threads.
  const auto local_calls = calls_on_this_thread(entry.get());
  entry->cv.wait(lock, [&entry, local_calls] {
    return entry->active_calls <= local_calls;
  });
}

void EventBus::publish(Event event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    event.sequence = next_sequence_++;
    if (event.received_at.time_since_epoch().count() == 0)
      event.received_at = std::chrono::steady_clock::now();
    if (event.timestamp.time_since_epoch().count() == 0)
      event.timestamp = event.received_at;
    if (event.trace_id.empty() && !event.session_id.empty() &&
        !event.turn_id.empty())
      event.trace_id = event.session_id + ":" + event.turn_id;
  }
  (void)deliver(event);
}

std::size_t EventBus::deliver(const Event& event) {
  std::vector<std::shared_ptr<HandlerEntry>> handlers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers.reserve(handlers_.size());
    for (const auto& entry : handlers_) handlers.push_back(entry.second);
  }
  std::size_t failures = 0;
  for (const auto& entry : handlers) {
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      if (!entry->enabled) continue;
      ++entry->active_calls;
    }
    ActiveHandler frame{entry.get(), active_handler};
    active_handler = &frame;
    try {
      entry->handler(event);
    } catch (...) {
      // Telemetry consumers must not break the realtime control path.
      ++failures;
    }
    active_handler = frame.previous;
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      --entry->active_calls;
      if (!entry->enabled) entry->cv.notify_all();
    }
  }
  return failures;
}

}  // namespace byteturn
