#include "byteturn/observability.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>

namespace byteturn {
namespace {

std::string metric_name(std::string name) {
  for (char& c : name) {
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == ':';
    if (!valid) c = '_';
  }
  return name;
}

std::string json_escape(const std::string& input) {
  std::string output;
  for (const unsigned char c : input) {
    switch (c) {
      case '"': output += "\\\""; break;
      case '\\': output += "\\\\"; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (c >= 0x20) output.push_back(static_cast<char>(c));
    }
  }
  return output;
}

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

void MetricsRegistry::increment(const std::string& name, std::uint64_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  counters_[metric_name(name)] += value;
}

void MetricsRegistry::observe(const std::string& name, double value,
                              const std::vector<double>& bounds) {
  if (!std::isfinite(value)) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& histogram = histograms_[metric_name(name)];
  if (histogram.bounds.empty()) {
    histogram.bounds = bounds;
    std::sort(histogram.bounds.begin(), histogram.bounds.end());
    histogram.bounds.erase(
        std::unique(histogram.bounds.begin(), histogram.bounds.end()),
        histogram.bounds.end());
    histogram.bucket_counts.resize(histogram.bounds.size() + 1);
  }
  ++histogram.count;
  histogram.sum += value;
  const auto it = std::lower_bound(histogram.bounds.begin(),
                                   histogram.bounds.end(), value);
  ++histogram.bucket_counts[static_cast<std::size_t>(it - histogram.bounds.begin())];
}

std::uint64_t MetricsRegistry::counter(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = counters_.find(metric_name(name));
  return it == counters_.end() ? 0 : it->second;
}

HistogramSnapshot MetricsRegistry::histogram(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = histograms_.find(metric_name(name));
  if (it == histograms_.end()) return {};
  return {it->second.count, it->second.sum, it->second.bounds,
          it->second.bucket_counts};
}

std::string MetricsRegistry::prometheus_text() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::ostringstream out;
  out << std::setprecision(12);
  for (const auto& entry : counters_) out << entry.first << ' ' << entry.second << '\n';
  for (const auto& entry : histograms_) {
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < entry.second.bucket_counts.size(); ++i) {
      cumulative += entry.second.bucket_counts[i];
      out << entry.first << "_bucket{le=\"";
      if (i == entry.second.bounds.size()) out << "+Inf";
      else out << entry.second.bounds[i];
      out << "\"} " << cumulative << '\n';
    }
    out << entry.first << "_sum " << entry.second.sum << '\n';
    out << entry.first << "_count " << entry.second.count << '\n';
  }
  return out.str();
}

RuntimeObserver::RuntimeObserver(EventBus& events, MetricsRegistry& metrics)
    : events_(events), metrics_(metrics),
      subscription_(events_.subscribe([this](const Event& event) { on_event(event); })) {}

RuntimeObserver::~RuntimeObserver() { events_.unsubscribe(subscription_); }

std::string RuntimeObserver::key(const Event& event, const std::string& phase) {
  return event.session_id + '\x1f' + event.turn_id + '\x1f' + phase + '\x1f' +
         event.name;
}

void RuntimeObserver::on_event(const Event& event) {
  metrics_.increment(std::string("byteturn_events_") + event_type_name(event.type) +
                     "_total");
  const auto finish = [&](const std::string& phase, const std::string& metric) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = starts_.find(key(event, phase));
    if (it == starts_.end()) return;
    metrics_.observe(metric, elapsed_ms(it->second, event.timestamp));
    starts_.erase(it);
  };
  if (event.type == EventType::TurnStarted || event.type == EventType::ModelStarted ||
      event.type == EventType::ToolStarted) {
    const char* phase = event.type == EventType::TurnStarted ? "turn" :
                        event.type == EventType::ModelStarted ? "model" : "tool";
    std::lock_guard<std::mutex> lock(mutex_);
    starts_[key(event, phase)] = event.timestamp;
    if (event.type == EventType::ModelStarted)
      starts_[key(event, "first_token")] = event.timestamp;
  } else if (event.type == EventType::AsrEndOfUtterance) {
    std::lock_guard<std::mutex> lock(mutex_);
    starts_[key(event, "asr_final")] = event.timestamp;
    starts_[key(event, "s2s_first_audio")] = event.timestamp;
    starts_[key(event, "conversation_turn")] = event.timestamp;
  } else if (event.type == EventType::InputSpeechEnded) {
    std::lock_guard<std::mutex> lock(mutex_);
    starts_[key(event, "duplex_first_audio")] = event.timestamp;
    starts_[key(event, "duplex_first_audible")] = event.timestamp;
    starts_[key(event, "conversation_turn")] = event.timestamp;
  } else if (event.type == EventType::TranscriptFinal) {
    finish("asr_final", "byteturn_asr_final_latency_ms");
  } else if (event.type == EventType::TurnCompleted) {
    finish("turn", "byteturn_turn_duration_ms");
  } else if (event.type == EventType::ModelCompleted) {
    finish("model", "byteturn_model_duration_ms");
  } else if (event.type == EventType::FirstToken) {
    finish("first_token", "byteturn_time_to_first_token_ms");
  } else if (event.type == EventType::SpeechStarted) {
    std::lock_guard<std::mutex> lock(mutex_);
    starts_[key(event, "tts_first_audio")] = event.timestamp;
    starts_[key(event, "tts_total")] = event.timestamp;
  } else if (event.type == EventType::FirstAudio) {
    finish("tts_first_audio", "byteturn_tts_first_audio_latency_ms");
    finish("s2s_first_audio", "byteturn_s2s_first_audio_latency_ms");
  } else if (event.type == EventType::AudioOutput) {
    Event session_event = event;
    session_event.turn_id.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = starts_.find(key(session_event, "duplex_first_audio"));
    if (it != starts_.end()) {
      metrics_.observe("byteturn_s2s_first_audio_latency_ms",
                       elapsed_ms(it->second, event.timestamp));
      starts_.erase(it);
    }
  } else if (event.type == EventType::PlaybackStarted) {
    Event session_event = event;
    session_event.turn_id.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = starts_.find(key(session_event, "duplex_first_audible"));
    if (it != starts_.end()) {
      metrics_.observe("byteturn_s2s_first_audible_latency_ms",
                       elapsed_ms(it->second, event.timestamp));
      starts_.erase(it);
    }
  } else if (event.type == EventType::BargeInDetected) {
    std::lock_guard<std::mutex> lock(mutex_);
    starts_[key(event, "barge_in_stop")] = event.timestamp;
  } else if (event.type == EventType::OutputCancelled) {
    finish("barge_in_stop", "byteturn_barge_in_stop_latency_ms");
  } else if (event.type == EventType::SpeechCompleted) {
    finish("tts_total", "byteturn_tts_total_duration_ms");
  } else if (event.type == EventType::ConversationTurnCompleted) {
    Event session_event = event;
    session_event.turn_id.clear();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto it = starts_.find(key(session_event, "conversation_turn"));
      if (it != starts_.end()) {
        metrics_.observe("byteturn_conversation_turn_duration_ms",
                         elapsed_ms(it->second, event.timestamp));
        starts_.erase(it);
        return;
      }
    }
    finish("conversation_turn", "byteturn_conversation_turn_duration_ms");
  } else if (event.type == EventType::ToolCompleted) {
    finish("tool", "byteturn_tool_duration_ms");
  } else if (event.type == EventType::Error) {
    metrics_.increment("byteturn_errors_total");
  } else if (event.type == EventType::TurnCancelled) {
    metrics_.increment("byteturn_turns_cancelled_total");
  }
}

JsonEventLogger::JsonEventLogger(EventBus& events, std::ostream& output,
                                 EventLogOptions options)
    : events_(events), output_(output), options_(options),
      subscription_(events_.subscribe([this](const Event& event) { write(event); })) {}

JsonEventLogger::~JsonEventLogger() { events_.unsubscribe(subscription_); }

void JsonEventLogger::write(const Event& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  output_ << "{\"sequence\":" << event.sequence << ",\"event\":\""
          << event_type_name(event.type) << "\",\"session_id\":\""
          << json_escape(event.session_id) << "\",\"turn_id\":\""
          << json_escape(event.turn_id) << "\",\"name\":\""
          << json_escape(event.name) << "\",\"trace_id\":\""
          << json_escape(event.trace_id) << '"';
  if (options_.include_payload)
    output_ << ",\"data\":\"" << json_escape(event.data) << '"';
  output_ << "}\n";
}

const char* event_type_name(EventType type) {
  switch (type) {
    case EventType::RealtimeSessionStarted: return "realtime_session_started";
    case EventType::InputSpeechStarted: return "input_speech_started";
    case EventType::InputSpeechEnded: return "input_speech_ended";
    case EventType::AsrEndOfUtterance: return "asr_end_of_utterance";
    case EventType::TranscriptPartial: return "transcript_partial";
    case EventType::TranscriptFinal: return "transcript_final";
    case EventType::TurnStarted: return "turn_started";
    case EventType::ModelStarted: return "model_started";
    case EventType::FirstToken: return "first_token";
    case EventType::ModelTextDelta: return "model_text_delta";
    case EventType::ModelCompleted: return "model_completed";
    case EventType::ToolStarted: return "tool_started";
    case EventType::ToolCompleted: return "tool_completed";
    case EventType::SpeechStarted: return "speech_started";
    case EventType::TtsChunkStarted: return "tts_chunk_started";
    case EventType::FirstAudio: return "first_audio";
    case EventType::AudioOutput: return "audio_output";
    case EventType::PlaybackStarted: return "playback_started";
    case EventType::PlaybackProgress: return "playback_progress";
    case EventType::BargeInDetected: return "barge_in_detected";
    case EventType::OutputCancelled: return "output_cancelled";
    case EventType::SpeechCompleted: return "speech_completed";
    case EventType::ConversationTurnCompleted: return "conversation_turn_completed";
    case EventType::TurnCompleted: return "turn_completed";
    case EventType::TurnCancelled: return "turn_cancelled";
    case EventType::RealtimeSessionClosed: return "realtime_session_closed";
    case EventType::Error: return "error";
  }
  return "unknown";
}

}  // namespace byteturn
