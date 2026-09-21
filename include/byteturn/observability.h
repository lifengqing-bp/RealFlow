#pragma once

#include "byteturn/event_timeline.h"

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace byteturn {

struct HistogramSnapshot {
  std::uint64_t count = 0;
  double sum = 0.0;
  std::vector<double> bounds;
  std::vector<std::uint64_t> bucket_counts;
};

class MetricsRegistry {
 public:
  void increment(const std::string& name, std::uint64_t value = 1);
  void observe(const std::string& name, double value,
               const std::vector<double>& bounds =
                   {5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000});
  std::uint64_t counter(const std::string& name) const;
  HistogramSnapshot histogram(const std::string& name) const;
  std::string prometheus_text() const;

 private:
  struct Histogram {
    std::uint64_t count = 0;
    double sum = 0.0;
    std::vector<double> bounds;
    std::vector<std::uint64_t> bucket_counts;
  };
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> counters_;
  std::map<std::string, Histogram> histograms_;
};

// Deterministic reducer: metrics use event.timestamp, never observer wall time.
// Use one ordered, canonical event stream; thread safety does not order a legacy
// EventBus's concurrent publications. Notification loss remains observable via
// EventTimeline::stats(), and cannot be repaired by inventing missing samples.
class RuntimeObserver {
 public:
  explicit RuntimeObserver(MetricsRegistry& metrics,
                           std::size_t max_pending_spans = 4096);
  RuntimeObserver(EventBus& events, MetricsRegistry& metrics,
                  std::size_t max_pending_spans = 4096);
  RuntimeObserver(EventTimeline& timeline, MetricsRegistry& metrics,
                  std::size_t max_pending_spans = 4096);
  ~RuntimeObserver();
  RuntimeObserver(const RuntimeObserver&) = delete;
  RuntimeObserver& operator=(const RuntimeObserver&) = delete;

  // Synchronous observation only; suitable for deterministic fixture/replay.
  // Replay into a fresh registry, not into one already fed by notifications.
  void observe(const Event& event);
  std::size_t pending_spans() const;

 private:
  // Session incarnation is part of identity. Operation distinguishes model
  // steps and the currently serialized tool calls; never put IDs in metric names.
  using Key = std::tuple<std::string, std::uint64_t, std::string,
                         std::string, std::string>;
  static Key key(const Event& event, const std::string& phase,
                 bool operation = false);
  void begin(const Event& event, const std::string& phase, bool operation = false);
  bool finish(const Event& event, const std::string& phase,
              const char* metric, bool operation = false, bool required = true);
  void discard(const Event& event, bool entire_session, bool agent_only = false);

  MetricsRegistry& metrics_;
  const std::size_t max_pending_spans_;
  EventBus* events_ = nullptr;
  EventTimeline* timeline_ = nullptr;
  EventBus::Subscription subscription_ = 0;
  mutable std::mutex mutex_;
  std::map<Key, std::chrono::steady_clock::time_point> starts_;
};

struct EventLogOptions {
  bool include_payload = false;
};

// Stable process-local timing fields; timestamps are monotonic nanoseconds,
// not UTC and not portable across processes without clock mapping.
std::string event_json(const Event& event, EventLogOptions options = {});

// Emits one JSON object per event. Payloads are excluded by default because
// they can contain transcripts, prompts, tool arguments, or model output.
class JsonEventLogger {
 public:
  JsonEventLogger(EventBus& events, std::ostream& output,
                  EventLogOptions options = {});
  ~JsonEventLogger();
  JsonEventLogger(const JsonEventLogger&) = delete;
  JsonEventLogger& operator=(const JsonEventLogger&) = delete;

 private:
  void write(const Event& event);
  EventBus& events_;
  std::ostream& output_;
  EventLogOptions options_;
  EventBus::Subscription subscription_;
  std::mutex mutex_;
};

const char* event_type_name(EventType type);

}  // namespace byteturn

