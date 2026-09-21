#pragma once

#include "byteturn/event.h"

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
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

class RuntimeObserver {
 public:
  RuntimeObserver(EventBus& events, MetricsRegistry& metrics);
  ~RuntimeObserver();
  RuntimeObserver(const RuntimeObserver&) = delete;
  RuntimeObserver& operator=(const RuntimeObserver&) = delete;

 private:
  void on_event(const Event& event);
  static std::string key(const Event& event, const std::string& phase);

  EventBus& events_;
  MetricsRegistry& metrics_;
  EventBus::Subscription subscription_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> starts_;
};

struct EventLogOptions {
  bool include_payload = false;
};

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

