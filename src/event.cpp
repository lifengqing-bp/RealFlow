#include "byteturn/event.h"

#include <vector>

namespace byteturn {

EventBus::Subscription EventBus::subscribe(Handler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = next_subscription_++;
  handlers_.emplace(id, std::move(handler));
  return id;
}

void EventBus::unsubscribe(Subscription subscription) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.erase(subscription);
}

void EventBus::publish(Event event) {
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    event.sequence = next_sequence_++;
    if (event.timestamp.time_since_epoch().count() == 0)
      event.timestamp = std::chrono::steady_clock::now();
    handlers.reserve(handlers_.size());
    for (const auto& entry : handlers_) handlers.push_back(entry.second);
  }
  for (const auto& handler : handlers) {
    try {
      handler(event);
    } catch (...) {
      // Telemetry consumers must not break the realtime control path.
    }
  }
}

}  // namespace byteturn
