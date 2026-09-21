#include "byteturn/observability.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <stdexcept>

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
        else {
          const char* hex = "0123456789abcdef";
          output += "\\u00";
          output.push_back(hex[c >> 4]);
          output.push_back(hex[c & 15]);
        }
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
  out.imbue(std::locale::classic());
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

RuntimeObserver::RuntimeObserver(MetricsRegistry& metrics, std::size_t max_pending_spans)
    : metrics_(metrics), max_pending_spans_(max_pending_spans) {
  if (!max_pending_spans_) throw std::invalid_argument("observer span limit must be positive");
}
RuntimeObserver::RuntimeObserver(EventBus& events, MetricsRegistry& metrics,
                                 std::size_t max_pending_spans)
    : RuntimeObserver(metrics, max_pending_spans) {
  events_ = &events;
  // Subscribe only after every member (especially the mutex/map) is initialized.
  subscription_ = events.subscribe([this](const Event& e) { observe(e); });
}
RuntimeObserver::RuntimeObserver(EventTimeline& timeline, MetricsRegistry& metrics,
                                 std::size_t max_pending_spans)
    : RuntimeObserver(metrics, max_pending_spans) {
  timeline_ = &timeline;
  subscription_ = timeline.subscribe([this](const Event& e) { observe(e); });
}
RuntimeObserver::~RuntimeObserver() {
  if (timeline_) timeline_->unsubscribe(subscription_);
  if (events_) events_->unsubscribe(subscription_);
}
RuntimeObserver::Key RuntimeObserver::key(const Event& e, const std::string& phase,
                                          bool operation) {
  return {e.session_id, e.generation, e.turn_id, phase, operation ? e.name : ""};
}
std::size_t RuntimeObserver::pending_spans() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return starts_.size();
}
void RuntimeObserver::begin(const Event& e, const std::string& phase, bool operation) {
  const auto k = key(e, phase, operation);
  auto found = starts_.find(k);
  if (found != starts_.end()) {
    // Ambiguous duplicate starts must not silently replace the original time.
    found->second = {};
    metrics_.increment("byteturn_observer_duplicate_starts_total");
    return;
  }
  if (starts_.size() >= max_pending_spans_) {
    metrics_.increment("byteturn_observer_span_limit_rejections_total");
    return;
  }
  starts_.emplace(k, e.timestamp);
}
bool RuntimeObserver::finish(const Event& e, const std::string& phase,
                              const char* metric, bool operation, bool required) {
  auto it = starts_.find(key(e, phase, operation));
  if (it == starts_.end()) {
    if (required) metrics_.increment("byteturn_observer_unmatched_endpoints_total");
    return false;
  }
  const auto start = it->second;
  starts_.erase(it);
  if (start.time_since_epoch().count() == 0 ||
      e.timestamp.time_since_epoch().count() == 0 || e.timestamp < start) {
    metrics_.increment("byteturn_observer_invalid_intervals_total");
  } else {
    metrics_.observe(metric, elapsed_ms(start, e.timestamp));
  }
  return true;
}
void RuntimeObserver::discard(const Event& e, bool entire_session, bool agent_only) {
  std::uint64_t count = 0;
  for (auto it = starts_.begin(); it != starts_.end();) {
    const auto& k = it->first;
    const auto& phase = std::get<3>(k);
    const bool agent = phase == "turn" || phase == "model" ||
                       phase == "first_token" || phase == "tool";
    if (std::get<0>(k) == e.session_id && std::get<1>(k) == e.generation &&
        (entire_session || std::get<2>(k) == e.turn_id) && (!agent_only || agent)) {
      it = starts_.erase(it);
      ++count;
    } else ++it;
  }
  if (count) metrics_.increment("byteturn_observer_discarded_spans_total", count);
}
void RuntimeObserver::observe(const Event& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  metrics_.increment(std::string("byteturn_events_") + event_type_name(event.type) + "_total");
  switch (event.type) {
    case EventType::TurnStarted:
      begin(event, "turn");
      break;
    case EventType::ModelStarted:
      begin(event, "model", true);
      begin(event, "first_token", true);
      break;
    case EventType::ToolStarted:
      begin(event, "tool", true);
      break;
    case EventType::AsrEndOfUtterance:
      begin(event, "asr_final");
      begin(event, "s2s_first_audio");
      begin(event, "conversation_turn");
      break;
    case EventType::InputSpeechEnded:
      // Legacy native path has one uncorrelated input in flight. Multiple
      // speech ends invalidate the interval rather than choosing the latest.
      begin(event, "duplex_first_audio");
      begin(event, "duplex_first_audible");
      begin(event, "conversation_turn");
      break;
    case EventType::TranscriptFinal:
      finish(event, "asr_final", "byteturn_asr_final_latency_ms", false, false);
      break;
    case EventType::FirstToken:
      finish(event, "first_token", "byteturn_time_to_first_token_ms", true);
      break;
    case EventType::ModelCompleted:
      finish(event, "model", "byteturn_model_duration_ms", true);
      // Tool-only model steps legitimately have no first text token.
      starts_.erase(key(event, "first_token", true));
      break;
    case EventType::ToolCompleted:
      finish(event, "tool", "byteturn_tool_duration_ms", true);
      break;
    case EventType::TurnCompleted:
      finish(event, "turn", "byteturn_turn_duration_ms");
      discard(event, false, true);  // Speech may still be running.
      break;
    case EventType::TurnCancelled:
      metrics_.increment("byteturn_turns_cancelled_total");
      discard(event, false, true);
      break;
    case EventType::TurnFailed:
      metrics_.increment("byteturn_turns_failed_total");
      discard(event, false, true);
      break;
    case EventType::TtsChunkStarted:
      // First chunk submission, not generic/native SpeechStarted, is a TTS
      // measurement endpoint. Subsequent chunks belong to the same utterance.
      if (!starts_.count(key(event, "tts_total"))) {
        begin(event, "tts_first_audio");
        begin(event, "tts_total");
      }
      break;
    case EventType::FirstAudio:
      finish(event, "tts_first_audio", "byteturn_tts_first_audio_latency_ms");
      finish(event, "s2s_first_audio", "byteturn_s2s_first_audio_latency_ms", false, false);
      break;
    case EventType::AudioOutput:
    case EventType::PlaybackStarted: {
      Event input = event;
      input.turn_id.clear();  // Current native provider has no input/response link.
      const bool audio = event.type == EventType::AudioOutput;
      const char* phase = audio ? "duplex_first_audio" : "duplex_first_audible";
      const char* metric = audio ? "byteturn_s2s_first_audio_latency_ms"
                                 : "byteturn_s2s_first_audible_latency_ms";
      if (!finish(event, phase, metric, false, false) && !event.turn_id.empty())
        finish(input, phase, metric, false, false);
      break;
    }
    case EventType::BargeInDetected:
      begin(event, "barge_in_stop");
      break;
    case EventType::OutputCancelled:
      finish(event, "barge_in_stop", "byteturn_barge_in_stop_latency_ms", false, false);
      discard(event, false);
      break;
    case EventType::SpeechCompleted:
      finish(event, "tts_total", "byteturn_tts_total_duration_ms", false, false);
      break;
    case EventType::ConversationTurnCompleted: {
      if (!finish(event, "conversation_turn", "byteturn_conversation_turn_duration_ms",
                  false, false) && !event.turn_id.empty()) {
        Event input = event;
        input.turn_id.clear();
        finish(input, "conversation_turn", "byteturn_conversation_turn_duration_ms", false, false);
      }
      discard(event, false);
      break;
    }
    case EventType::ConversationTurnCancelled:
    case EventType::ConversationTurnFailed:
      discard(event, false);
      break;
    case EventType::SessionStopped:
    case EventType::SessionFailed:
    case EventType::RealtimeSessionClosed:
      discard(event, true);
      break;
    case EventType::Error:
      metrics_.increment("byteturn_errors_total");
      break;
    default: break;
  }
}

JsonEventLogger::JsonEventLogger(EventBus& events, std::ostream& output,
                                 EventLogOptions options)
    : events_(events), output_(output), options_(options), subscription_(0) {
  subscription_ = events_.subscribe([this](const Event& event) { write(event); });
}
JsonEventLogger::~JsonEventLogger() { events_.unsubscribe(subscription_); }

std::string event_json(const Event& event, EventLogOptions options) {
  const auto ns = [](auto point) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
  };
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "{\"schema_version\":1,\"sequence\":" << event.sequence
      << ",\"event\":\"" << event_type_name(event.type)
      << "\",\"session_id\":\"" << json_escape(event.session_id)
      << "\",\"generation\":" << event.generation
      << ",\"turn_id\":\"" << json_escape(event.turn_id)
      << "\",\"name\":\"" << json_escape(event.name)
      << "\",\"trace_id\":\"" << json_escape(event.trace_id)
      << "\",\"timestamp_ns\":" << ns(event.timestamp)
      << ",\"received_at_ns\":" << ns(event.received_at);
  if (options.include_payload) out << ",\"data\":\"" << json_escape(event.data) << '"';
  out << '}';
  return out.str();
}
void JsonEventLogger::write(const Event& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  output_ << event_json(event, options_) << '\n';
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
    case EventType::SessionStarted: return "session_started";
    case EventType::SessionStopped: return "session_stopped";
    case EventType::SessionFailed: return "session_failed";
    case EventType::ResponseCancelRequested: return "response_cancel_requested";
    case EventType::TurnFailed: return "turn_failed";
    case EventType::ConversationTurnCancelled: return "conversation_turn_cancelled";
    case EventType::ConversationTurnFailed: return "conversation_turn_failed";
  }
  return "unknown";
}

}  // namespace byteturn
