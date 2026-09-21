#include "byteturn/event_timeline.h"

namespace byteturn {

EventTimeline::EventTimeline(EventBus* bus) : bus_(bus ? bus : &owned_bus_) {}

void EventTimeline::append(Event event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
  }
  bus_->publish(std::move(event));
}

std::vector<Event> EventTimeline::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_;
}

std::size_t EventTimeline::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

}  // namespace byteturn
