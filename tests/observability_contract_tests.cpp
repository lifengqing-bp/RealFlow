#include "byteturn/observability.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/conversation_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using namespace byteturn;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
  if (!value) { std::cerr << "FAIL: " << message << std::endl; std::abort(); }
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_all();
    check(cv.wait_for(lock, 5s, [&] { return released; }), "gate release timeout");
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return entered; }), "gate entry timeout");
  }
  void open() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct Capture {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Event> events;
  void push(const Event& e) {
    std::lock_guard<std::mutex> lock(mutex);
    events.push_back(e); cv.notify_all();
  }
  void wait(EventType type) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] {
      return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.type == type; });
    }), "observable endpoint timeout");
  }
};
auto time_at(int ms) { return std::chrono::steady_clock::time_point{} + 1s + std::chrono::milliseconds(ms); }
Event event(EventType type, int ms, std::string turn = "t", std::string name = "") {
  Event e(type, "session", std::move(turn), 0, time_at(ms), std::move(name));
  e.generation = 7;
  return e;
}
std::size_t count(const std::vector<Event>& events, EventType type) {
  return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
      [&](const auto& e) { return e.type == type; }));
}
std::size_t index(const std::vector<Event>& events, EventType type) {
  auto it = std::find_if(events.begin(), events.end(), [&](const auto& e) { return e.type == type; });
  check(it != events.end(), "required event missing");
  return static_cast<std::size_t>(it - events.begin());
}
void metric(const MetricsRegistry& m, const char* name, std::uint64_t n, double sum) {
  const auto h = m.histogram(name);
  check(h.count == n && h.sum == sum, name);
}
void exact_intervals_and_replay() {
  const std::vector<Event> fixture{
    event(EventType::AsrEndOfUtterance, 0),
    event(EventType::TranscriptFinal, 20),
    event(EventType::TurnStarted, 20),
    event(EventType::ModelStarted, 20, "t", "0"),
    event(EventType::FirstToken, 27, "t", "0"),
    event(EventType::ModelCompleted, 50, "t", "0"),
    event(EventType::ToolStarted, 50, "t", "lookup"),
    event(EventType::ToolCompleted, 60, "t", "lookup"),
    event(EventType::TtsChunkStarted, 65),
    event(EventType::FirstAudio, 80),
    event(EventType::TurnCompleted, 85),
    event(EventType::SpeechCompleted, 120),
    event(EventType::ConversationTurnCompleted, 120)
  };
  MetricsRegistry first, second;
  RuntimeObserver a(first), b(second);
  for (auto e : fixture) { e.received_at = time_at(9999); a.observe(e); }
  for (auto e : fixture) { e.received_at = time_at(12345); b.observe(e); }
  check(first.prometheus_text() == second.prometheus_text(), "arrival time does not change source-time metrics");
  metric(first, "byteturn_asr_final_latency_ms", 1, 20);
  metric(first, "byteturn_time_to_first_token_ms", 1, 7);
  metric(first, "byteturn_model_duration_ms", 1, 30);
  metric(first, "byteturn_tool_duration_ms", 1, 10);
  metric(first, "byteturn_tts_first_audio_latency_ms", 1, 15);
  metric(first, "byteturn_tts_total_duration_ms", 1, 55);
  metric(first, "byteturn_s2s_first_audio_latency_ms", 1, 80);
  metric(first, "byteturn_turn_duration_ms", 1, 65);
  metric(first, "byteturn_conversation_turn_duration_ms", 1, 120);
  check(first.histogram("byteturn_time_to_first_token_ms").bucket_counts ==
        std::vector<std::uint64_t>({0,1,0,0,0,0,0,0,0,0,0}), "exact TTFT bucket");
  check(a.pending_spans() == 0 && b.pending_spans() == 0, "completed fixture has no dangling spans");
}
void incarnation_and_structured_keys() {
  MetricsRegistry m; RuntimeObserver o(m);
  auto a = event(EventType::ModelStarted, 10, "t", "0");
  auto b = a; b.generation = 8; b.timestamp = time_at(30);
  o.observe(a); o.observe(b);
  a.type = b.type = EventType::ModelCompleted;
  a.timestamp = time_at(50); b.timestamp = time_at(100);
  o.observe(a); o.observe(b);
  metric(m, "byteturn_model_duration_ms", 2, 110);
  check(o.pending_spans() == 0, "incarnations do not overwrite each other");
  // Delimiter-containing IDs must not alias concatenated string keys.
  a = event(EventType::TurnStarted, 0, "c"); a.session_id = "a\x1f" "b";
  b = event(EventType::TurnStarted, 10, "b\x1f" "c"); b.session_id = "a";
  o.observe(a); o.observe(b);
  a.type = b.type = EventType::TurnCompleted;
  a.timestamp = time_at(20); b.timestamp = time_at(40);
  o.observe(a); o.observe(b);
  metric(m, "byteturn_turn_duration_ms", 2, 50);
}
void invalid_or_missing_endpoints_are_not_zero_samples() {
  MetricsRegistry m; RuntimeObserver o(m);
  o.observe(event(EventType::TurnCompleted, 5));
  o.observe(event(EventType::TurnStarted, 20));
  o.observe(event(EventType::TurnCompleted, 10));
  o.observe(event(EventType::TurnStarted, 30));
  o.observe(event(EventType::TurnStarted, 40));
  o.observe(event(EventType::TurnCompleted, 50));
  auto zero = event(EventType::TurnStarted, 0); zero.timestamp = {};
  o.observe(zero); o.observe(event(EventType::TurnCompleted, 70));
  metric(m, "byteturn_turn_duration_ms", 0, 0);
  check(m.counter("byteturn_observer_unmatched_endpoints_total") == 1, "missing endpoint diagnostic");
  check(m.counter("byteturn_observer_duplicate_starts_total") == 1, "ambiguous start diagnostic");
  check(m.counter("byteturn_observer_invalid_intervals_total") == 3, "invalid interval diagnostic");
  check(o.pending_spans() == 0, "invalid intervals are retired");
  o.observe(event(EventType::TurnStarted, 100)); o.observe(event(EventType::TurnCompleted, 100));
  metric(m, "byteturn_turn_duration_ms", 1, 0); // True zero, not missing.
  o.observe(event(EventType::TurnCompleted, 110));
  metric(m, "byteturn_turn_duration_ms", 1, 0); // No duplicate duration.
}
void cancellation_scope_bounds_and_cleanup() {
  MetricsRegistry m; RuntimeObserver o(m, 4);
  o.observe(event(EventType::AsrEndOfUtterance, 0));
  o.observe(event(EventType::TurnStarted, 1));
  o.observe(event(EventType::ModelStarted, 2, "t", "0"));
  check(o.pending_spans() == 4, "span capacity enforced");
  check(m.counter("byteturn_observer_span_limit_rejections_total") == 2, "capacity loss explicit");
  o.observe(event(EventType::ResponseCancelRequested, 3, "", "all_responses"));
  check(o.pending_spans() == 4, "cancellation request is not task completion");
  o.observe(event(EventType::TurnCancelled, 4, "t", "cancelled"));
  check(o.pending_spans() == 3, "agent cancellation does not finish speech");
  o.observe(event(EventType::ConversationTurnCancelled, 5, "t", "response_terminated"));
  check(o.pending_spans() == 0, "response cancellation clears its span state regardless of name");
  metric(m, "byteturn_turn_duration_ms", 0, 0);
  metric(m, "byteturn_conversation_turn_duration_ms", 0, 0);
  o.observe(event(EventType::ModelStarted, 10, "t", "0"));
  auto newer = event(EventType::TurnStarted, 20); newer.generation = 8; o.observe(newer);
  o.observe(event(EventType::SessionStopped, 30, "", "stopped"));
  check(o.pending_spans() == 1, "old session terminal cannot clear replacement spans");
  newer.type = EventType::TurnCompleted; newer.timestamp = time_at(40); o.observe(newer);
  metric(m, "byteturn_turn_duration_ms", 1, 20);
}
void native_audio_does_not_invent_tts_or_playback() {
  MetricsRegistry m; RuntimeObserver o(m);
  o.observe(event(EventType::InputSpeechEnded, 10, ""));
  o.observe(event(EventType::SpeechStarted, 20, "response"));
  o.observe(event(EventType::AudioOutput, 30, "response"));
  o.observe(event(EventType::AudioOutput, 35, "response"));
  metric(m, "byteturn_s2s_first_audio_latency_ms", 1, 20);
  metric(m, "byteturn_s2s_first_audible_latency_ms", 0, 0);
  metric(m, "byteturn_tts_first_audio_latency_ms", 0, 0);
  o.observe(event(EventType::PlaybackStarted, 40, "response"));
  metric(m, "byteturn_s2s_first_audible_latency_ms", 1, 30);
  o.observe(event(EventType::ConversationTurnCompleted, 50, "response"));
  metric(m, "byteturn_conversation_turn_duration_ms", 1, 40);
  check(o.pending_spans() == 0, "native fixture does not leak synthetic TTS spans");
}
void canonical_clock_json_and_redaction() {
  auto c = TimelineConfig{}; c.session_id = "s"; c.generation = 42;
  c.clock = [] { return time_at(100); };
  EventTimeline timeline(nullptr, c);
  std::ostringstream logs;
  JsonEventLogger logger(timeline.bus(), logs);
  Event raw(EventType::TranscriptFinal, {}, "t", 99, {}, "safe", "private transcript");
  Event admitted(EventType::Error);
  check(bool(timeline.append(raw, &admitted)), "canonical event admitted");
  check(timeline.flush(), "canonical logger drained");
  check(admitted.sequence == 1 && admitted.generation == 42 &&
        admitted.timestamp == time_at(100) && admitted.received_at == time_at(100), "controlled admission metadata");
  check(event_json(admitted) + '\n' == logs.str(), "stored and delivered JSON match exactly");
  check(logs.str().find("\"schema_version\":1") != std::string::npos &&
        logs.str().find("\"generation\":42") != std::string::npos &&
        logs.str().find("\"timestamp_ns\":1100000000") != std::string::npos &&
        logs.str().find("\"received_at_ns\":1100000000") != std::string::npos, "machine-readable clock and identity fields");
  check(logs.str().find("private transcript") == std::string::npos &&
        logs.str().find("\"data\"") == std::string::npos, "payload redacted by default");
  admitted.name = std::string("a\x01") + "\"\\\n";
  check(event_json(admitted).find("a\\u0001\\\"\\\\\\n") != std::string::npos, "control characters escaped without losing identity");
  check(event_json(admitted, {true}).find("private transcript") != std::string::npos, "explicit payload opt-in");
}
void notification_loss_is_not_a_complete_trace() {
  Gate gate;
  TimelineConfig c; c.max_pending_notifications = 1;
  EventTimeline timeline(nullptr, c);
  MetricsRegistry live; RuntimeObserver observer(timeline, live);
  auto sub = timeline.subscribe([&](const Event& e) { if (e.sequence == 1) gate.block(); });
  auto a = event(EventType::ModelStarted, 10, "t", "0"); a.generation = 1;
  timeline.append(a); gate.wait();
  a.type = EventType::FirstToken; a.timestamp = time_at(20); timeline.append(a);
  a.type = EventType::ModelCompleted; a.timestamp = time_at(30);
  auto dropped = timeline.append(a);
  check(bool(dropped) && !dropped.notification_enqueued, "journal accepted dropped notification");
  gate.open(); check(timeline.flush(), "delivered notifications drained");
  timeline.unsubscribe(sub);
  const auto stats = timeline.stats();
  check(stats.accepted == 3 && stats.dropped_notifications == 1 && stats.evicted == 0 &&
        stats.first_dropped_sequence == 3 && stats.last_dropped_sequence == 3, "deterministic notification gap");
  metric(live, "byteturn_model_duration_ms", 0, 0);
  MetricsRegistry replay; RuntimeObserver offline(replay);
  for (const auto& e : timeline.snapshot()) offline.observe(e);
  metric(replay, "byteturn_model_duration_ms", 1, 20);
  check(offline.pending_spans() == 0, "complete retained trace replays independently of live delivery");
  check(live.prometheus_text() != replay.prometheus_text(), "loss cannot masquerade as complete live metrics");
}
void evicted_start_cannot_make_a_duration() {
  TimelineConfig c; c.max_events = 1;
  EventTimeline t(nullptr, c);
  auto e = event(EventType::TurnStarted, 10); e.generation = 1; t.append(e);
  e.type = EventType::TurnCompleted; e.timestamp = time_at(20); t.append(e);
  check(t.stats().evicted == 1 && t.stats().oldest_sequence == 2, "retention gap visible");
  MetricsRegistry m; RuntimeObserver o(m);
  for (const auto& retained : t.snapshot()) o.observe(retained);
  metric(m, "byteturn_turn_duration_ms", 0, 0);
  check(m.counter("byteturn_observer_unmatched_endpoints_total") == 1, "truncated trace reports missing start");
}
class Input final : public AsrProvider {
 public:
  void reset() override {}
  void push(const AudioFrame& f, const std::function<void(std::string, bool)>& cb) override {
    if (f.end_of_utterance) cb("calculate", true);
  }
};
class Speech final : public TtsProvider {
 public:
  void cancel() override {}
  void synthesize(const std::string&, const std::function<bool(const AudioFrame&)>& cb) override {
    cb({{42}, 16000, 1, false});
  }
};
void pipeline_trace_is_an_executable_contract() {
  class Model final : public LlmProvider {
   public:
    Gate final;
    LlmTurn complete(const std::vector<Message>&) override { return {}; }
    LlmTurn stream(const std::vector<Message>& h, const TextDeltaSink& cb,
                   const std::function<bool()>&) override {
      if (h.back().role != Role::Tool) return {"", {{"c", "double", "21"}}, true};
      cb("The answer is forty-two!"); final.block();
      return {"The answer is forty-two!", {}, true};
    }
  } model;
  Input input; Speech speech; ToolRegistry tools;
  tools.add("double", [](const std::string& s) { return std::to_string(std::stoi(s) * 2); });
  Agent agent(model, tools); // No manually wired Agent event bus.
  SessionExecutor executor(1); AsyncSession legacy("s", agent, executor);
  std::vector<int> output;
  auto engine = std::make_unique<PipelineConversationEngine>(input, legacy, speech,
      [](const std::string&, bool) {}, [&](const AudioFrame& f) { output.push_back(f.samples.front()); });
  ConversationSession session("s", std::move(engine));
  Capture captured; auto sub = session.timeline().subscribe([&](const Event& e) { captured.push(e); });
  MetricsRegistry live; RuntimeObserver observer(session.timeline(), live);
  session.start(); session.push_audio({{1}, 16000, 1, true});
  model.final.wait(); captured.wait(EventType::FirstAudio);
  model.final.open(); captured.wait(EventType::ConversationTurnCompleted);
  session.stop(); check(session.timeline().flush(), "pipeline trace drained");
  session.timeline().unsubscribe(sub);
  const auto trace = session.timeline().snapshot();
  check(trace.size() == captured.events.size(), "journal and notification event counts agree");
  for (std::size_t i = 0; i < trace.size(); ++i) {
    check(event_json(trace[i], {true}) == event_json(captured.events[i], {true}), "canonical delivery matches retained data");
    check(trace[i].sequence == i + 1 && trace[i].session_id == "s" && trace[i].generation == 1, "pipeline identity and sequence");
    if (!trace[i].turn_id.empty()) check(trace[i].trace_id == "s:1", "pipeline correlation consistent");
  }
  for (auto type : {EventType::TurnStarted, EventType::TurnCompleted, EventType::FirstToken,
                    EventType::ToolStarted, EventType::ToolCompleted, EventType::SpeechCompleted,
                    EventType::ConversationTurnCompleted}) check(count(trace, type) == 1, "required endpoint exactly once");
  check(count(trace, EventType::ModelStarted) == 2 && count(trace, EventType::ModelCompleted) == 2, "two model steps observed once each");
  check(index(trace, EventType::AsrEndOfUtterance) < index(trace, EventType::TranscriptFinal) &&
        index(trace, EventType::ToolStarted) < index(trace, EventType::ToolCompleted) &&
        index(trace, EventType::FirstAudio) < index(trace, EventType::TurnCompleted) &&
        index(trace, EventType::TurnCompleted) < index(trace, EventType::ConversationTurnCompleted), "required causal order, not arbitrary thread interleaving");
  check(output == std::vector<int>({42}), "audio result");
  MetricsRegistry replay; RuntimeObserver offline(replay);
  for (const auto& e : trace) offline.observe(e);
  check(live.prometheus_text() == replay.prometheus_text(), "actual pipeline replays identical metrics");
  check(observer.pending_spans() == 0 && offline.pending_spans() == 0, "pipeline metric state fully retired");
  check(live.histogram("byteturn_model_duration_ms").count == 2 &&
        live.histogram("byteturn_tool_duration_ms").count == 1 &&
        live.histogram("byteturn_tts_first_audio_latency_ms").count == 1, "real pipeline metrics cover all stages");
  const auto quality = session.timeline().stats();
  check(quality.dropped_notifications == 0 && quality.evicted == 0 && quality.rejected == 0 &&
        quality.observer_failures == 0 && quality.notification_allocation_failures == 0,
        "fixture has complete evidence");
}
void cancellation_has_request_and_terminal_evidence() {
  class Model final : public LlmProvider {
   public:
    Gate blocked; bool accepted = true;
    LlmTurn complete(const std::vector<Message>&) override { return {}; }
    LlmTurn stream(const std::vector<Message>&, const TextDeltaSink& cb,
                   const std::function<bool()>&) override {
      cb("This initial answer can be interrupted!"); blocked.block();
      accepted = cb("This stale token must be rejected!");
      return {"obsolete", {{"late", "effect", "x"}}, true};
    }
  } model;
  Input input; Speech speech; ToolRegistry tools; int effects = 0;
  tools.add("effect", [&](const std::string&) { ++effects; return "done"; });
  Agent agent(model, tools); SessionExecutor executor(1); AsyncSession legacy("s", agent, executor);
  ConversationSession session("s", std::make_unique<PipelineConversationEngine>(input, legacy, speech,
      [](const std::string&, bool) {}, [](const AudioFrame&) {}));
  Capture captured; auto sub = session.timeline().subscribe([&](const Event& e) { captured.push(e); });
  session.start(); session.push_audio({{1}, 16000, 1, true});
  model.blocked.wait(); captured.wait(EventType::FirstAudio);
  check(session.cancel_response(), "cancel request accepted");
  model.blocked.open(); captured.wait(EventType::ConversationTurnCancelled);
  session.stop(); check(session.timeline().flush(), "cancel trace drained"); session.timeline().unsubscribe(sub);
  const auto t = session.timeline().snapshot();
  check(count(t, EventType::ResponseCancelRequested) == 1 && count(t, EventType::TurnCancelled) == 1 &&
        count(t, EventType::ConversationTurnCancelled) == 1, "request, agent cancellation and response cancellation are separate facts");
  check(count(t, EventType::TurnCompleted) == 0 && count(t, EventType::ConversationTurnCompleted) == 0 &&
        count(t, EventType::ToolStarted) == 0 && !model.accepted && effects == 0, "no false success or late tool effect");
  check(index(t, EventType::ResponseCancelRequested) < index(t, EventType::TurnCancelled) &&
        index(t, EventType::TurnCancelled) < index(t, EventType::ConversationTurnCancelled),
        "request precedes worker termination and response drain");
  check(count(t, EventType::ModelTextDelta) == 1, "stale model text absent from evidence");
  MetricsRegistry m; RuntimeObserver o(m); for (const auto& e : t) o.observe(e);
  metric(m, "byteturn_turn_duration_ms", 0, 0);
  metric(m, "byteturn_conversation_turn_duration_ms", 0, 0);
  check(o.pending_spans() == 0, "cancelled spans retired");
}
void failure_and_incomplete_stream_are_not_success() {
  class Broken final : public LlmProvider {
   public:
    bool incomplete = false;
    LlmTurn complete(const std::vector<Message>&) override {
      if (!incomplete) throw std::runtime_error("provider failed");
      return {"partial", {}, false};
    }
  } llm;
  ToolRegistry tools; EventBus default_bus, invocation; Agent a(llm, tools, 8, &default_bus);
  std::vector<Event> events; int unexpected = 0;
  invocation.subscribe([&](const Event& e) { events.push_back(e); });
  default_bus.subscribe([&](const Event&) { ++unexpected; });
  for (bool incomplete : {false, true}) {
    llm.incomplete = incomplete; events.clear();
    bool threw = false;
    try { a.run_streaming("hello", "s", "t", [](const std::string&) { return true; }, {}, {}, &invocation); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw && count(events, EventType::TurnStarted) == 1 && count(events, EventType::TurnFailed) == 1 &&
          count(events, EventType::TurnCompleted) == 0, "failed attempt has one truthful terminal");
    check(a.history().size() == 1 && unexpected == 0, "rollback and one scoped event destination");
    MetricsRegistry m; RuntimeObserver o(m); for (const auto& e : events) o.observe(e);
    check(o.pending_spans() == 0, "failed model span retired");
  }
}
void completed_agent_and_failed_speech_are_distinct() {
  class Answer final : public LlmProvider {
   public:
    LlmTurn complete(const std::vector<Message>&) override {
      return {"This response will fail during synthesis!", {}, true};
    }
  } model;
  class BrokenSpeech final : public TtsProvider {
   public:
    void cancel() override {}
    void synthesize(const std::string&, const std::function<bool(const AudioFrame&)>&) override {
      throw std::runtime_error("synthesis failed");
    }
  } speech;
  Input input; ToolRegistry tools; Agent agent(model, tools);
  SessionExecutor executor(1); AsyncSession legacy("s", agent, executor);
  ConversationSession session("s", std::make_unique<PipelineConversationEngine>(input, legacy, speech,
      [](const std::string&, bool) {}, [](const AudioFrame&) {}));
  Capture captured; auto sub = session.timeline().subscribe([&](const Event& e) { captured.push(e); });
  session.start(); session.push_audio({{1}, 16000, 1, true});
  captured.wait(EventType::ConversationTurnFailed);
  session.stop(); check(session.timeline().flush(), "synthesis failure trace drained");
  session.timeline().unsubscribe(sub);
  const auto trace = session.timeline().snapshot();
  check(count(trace, EventType::TurnCompleted) == 1 && count(trace, EventType::TurnFailed) == 0 &&
        count(trace, EventType::ConversationTurnFailed) == 1 &&
        count(trace, EventType::ConversationTurnCompleted) == 0 && count(trace, EventType::FirstAudio) == 0,
        "agent success does not claim successful speech or first audio");
  MetricsRegistry m; RuntimeObserver o(m); for (const auto& e : trace) o.observe(e);
  check(m.histogram("byteturn_turn_duration_ms").count == 1, "completed agent duration retained");
  metric(m, "byteturn_tts_first_audio_latency_ms", 0, 0);
  metric(m, "byteturn_conversation_turn_duration_ms", 0, 0);
  check(o.pending_spans() == 0, "failed response drains speech metric state");
}
void step_limit_is_an_agent_failure_not_task_success() {
  class CallsTool final : public LlmProvider {
   public:
    LlmTurn complete(const std::vector<Message>&) override {
      return {"", {{"id", "noop", ""}}, true};
    }
  } llm;
  ToolRegistry tools; tools.add("noop", [](const std::string&) { return "done"; });
  EventBus bus; std::vector<Event> trace;
  bus.subscribe([&](const Event& e) { trace.push_back(e); });
  Agent agent(llm, tools, 1, &bus);
  check(!agent.run("loop", "s", "t").empty(), "existing explanatory response preserved");
  check(count(trace, EventType::TurnStarted) == 1 && count(trace, EventType::TurnFailed) == 1 &&
        count(trace, EventType::TurnCompleted) == 0, "step limit has one failed terminal outcome");
}

void observer_construction_and_teardown_while_publishing() {
  EventBus bus; std::atomic<bool> stop{false};
  std::thread producer([&] { while (!stop.load()) bus.publish({EventType::Error}); });
  for (int i = 0; i < 50; ++i) {
    MetricsRegistry m; std::ostringstream logs;
    RuntimeObserver o(bus, m); JsonEventLogger logger(bus, logs);
  }
  stop = true; producer.join();
}
}  // namespace
int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests{
    {"exact intervals and replay", exact_intervals_and_replay},
    {"incarnation and structured identity", incarnation_and_structured_keys},
    {"invalid and missing intervals", invalid_or_missing_endpoints_are_not_zero_samples},
    {"cancellation scope and bounded cleanup", cancellation_scope_bounds_and_cleanup},
    {"native endpoint distinctions", native_audio_does_not_invent_tts_or_playback},
    {"canonical JSON and privacy", canonical_clock_json_and_redaction},
    {"notification loss evidence", notification_loss_is_not_a_complete_trace},
    {"retention loss evidence", evicted_start_cannot_make_a_duration},
    {"real pipeline observable contract", pipeline_trace_is_an_executable_contract},
    {"cancellation observable contract", cancellation_has_request_and_terminal_evidence},
    {"failure and incomplete-stream contract", failure_and_incomplete_stream_are_not_success},
    {"completed agent versus failed speech", completed_agent_and_failed_speech_are_distinct},
    {"step-limit terminal outcome", step_limit_is_an_agent_failure_not_task_success},
    {"observer construction and teardown", observer_construction_and_teardown_while_publishing}
  };
  for (const auto& t : tests) { t.second(); std::cout << "PASS " << t.first << std::endl; }
  std::cout << tests.size() << " observability contract tests passed\n";
}
