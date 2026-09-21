#pragma once

#include "byteturn/event.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace byteturn {

// Ordered, in-memory record of semantic interaction events for one session.
// EventBus remains the fan-out mechanism; EventTimeline is the session's
// inspectable source of truth and intentionally does not assume sequential
// turns.
class EventTimeline {
 public:
  using Handler = std::function<void(const Event&)>;

  explicit EventTimeline(EventBus* bus = nullptr);

  void append(Event event);
  std::vector<Event> snapshot() const;
  std::size_t size() const;

  EventBus& bus() { return *bus_; }
  const EventBus& bus() const { return *bus_; }

 private:
  mutable std::mutex mutex_;
  std::vector<Event> events_;
  EventBus owned_bus_;
  EventBus* bus_;
};

}  // namespace byteturn
