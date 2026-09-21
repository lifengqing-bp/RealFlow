#include "byteturn/event.h"

#include <vector>

namespace byteturn {
namespace {
thread_local const void* active_handler_entry = nullptr;
}

EventBus::Subscription EventBus::subscribe(Handler handler) {
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
  if (active_handler_entry == entry.get()) return;
  entry->cv.wait(lock, [&entry] { return entry->active_calls == 0; });
}

void EventBus::publish(Event event) {
  std::vector<std::shared_ptr<HandlerEntry>> handlers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    event.sequence = next_sequence_++;
    if (event.timestamp.time_since_epoch().count() == 0)
      event.timestamp = std::chrono::steady_clock::now();
    if (event.trace_id.empty() && !event.session_id.empty() &&
        !event.turn_id.empty())
      event.trace_id = event.session_id + ":" + event.turn_id;
    handlers.reserve(handlers_.size());
    for (const auto& entry : handlers_) handlers.push_back(entry.second);
  }
  for (const auto& entry : handlers) {
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      if (!entry->enabled) continue;
      ++entry->active_calls;
    }
    const void* previous_entry = active_handler_entry;
    active_handler_entry = entry.get();
    try {
      entry->handler(event);
    } catch (...) {
      // Telemetry consumers must not break the realtime control path.
    }
    active_handler_entry = previous_entry;
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      --entry->active_calls;
      if (!entry->enabled && entry->active_calls == 0) entry->cv.notify_all();
    }
  }
}

}  // namespace byteturn
