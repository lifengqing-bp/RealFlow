#include "test_support.h"
#include "byteturn/full_duplex_conversation.h"
#include "byteturn/observability.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/runtime_manager.h"

#include <atomic>
#include <set>

namespace {
using namespace byteturn;
using namespace realflow_test;

class Input final : public AsrProvider {
 public:
  std::atomic<int> resets{0};
  void reset() override { ++resets; }
  void push(const AudioFrame& frame, const std::function<void(std::string, bool)>& sink) override {
    const int marker = frame.samples.empty() ? 0 : frame.samples.front();
    sink(marker ? std::to_string(marker) : "", frame.end_of_utterance);
    { std::lock_guard<std::mutex> lock(mutex_); ++processed_; cv_.notify_all(); }
  }
  void wait(int n) {
    std::unique_lock<std::mutex> lock(mutex_);
    check(cv_.wait_for(lock, 5s, [&] { return processed_ >= n; }), "ASR processing timed out");
  }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  int processed_ = 0;
};
class Model final : public LlmProvider {
 public:
  std::function<LlmTurn(const std::vector<Message>&)> action;
  std::atomic<int> calls{0};
  LlmTurn complete(const std::vector<Message>& history) override {
    ++calls;
    if (action) return action(history);
    return {"The answer for input " + history.back().content + "!", {}, true};
  }
};
class Speech final : public TtsProvider {
 public:
  std::atomic<int> calls{0}, cancels{0};
  bool fail_first = false;
  void cancel() override { ++cancels; }
  void synthesize(const std::string&, const std::function<bool(const AudioFrame&)>& sink) override {
    const int call = ++calls;
    if (fail_first && call == 1) throw std::runtime_error("fixture TTS failure");
    sink({{42}, 16000, 1, false});
  }
};
class AudioCapture {
 public:
  void push(const AudioFrame& f) {
    std::lock_guard<std::mutex> lock(mutex_); frames_.push_back(f); cv_.notify_all();
  }
  void wait(std::size_t n) {
    std::unique_lock<std::mutex> lock(mutex_);
    check(cv_.wait_for(lock, 5s, [&] { return frames_.size() >= n; }), "audio timed out");
  }
  std::size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return frames_.size(); }
 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<AudioFrame> frames_;
};
struct Pipeline {
  Input input;
  Model model;
  Speech speech;
  ToolRegistry tools;
  Agent agent{model, tools};
  AsyncSession legacy;
  AudioCapture audio;
  EventCapture captured;
  EventBus events;
  Pipeline(const std::string& id, SessionExecutor& executor) : legacy(id, agent, executor) {
    events.subscribe([this](const Event& e) { captured.push(e); });
  }
  std::unique_ptr<ConversationEngine> engine() {
    return std::make_unique<PipelineConversationEngine>(input, legacy, speech,
        [](const std::string&, bool) {}, [&](const AudioFrame& f) { audio.push(f); });
  }
};
AudioFrame frame(int value, bool final = true) {
  return {{static_cast<std::int16_t>(value)}, 16000, 1, final};
}
void verify_trace(ConversationSession& session) {
  check(session.timeline().flush(), "trace drained");
  const auto events = session.timeline().snapshot();
  for (std::size_t i = 0; i < events.size(); ++i) {
    check(events[i].sequence == i + 1 && events[i].session_id == session.id() &&
          events[i].generation == 1, "canonical event identity/order");
    if (!events[i].turn_id.empty())
      check(events[i].trace_id == session.id() + ":" + events[i].turn_id, "turn correlation");
  }
  const auto stats = session.timeline().stats();
  check(stats.evicted == 0 && stats.dropped_notifications == 0 && stats.rejected == 0 &&
        stats.observer_failures == 0 && stats.notification_allocation_failures == 0,
        "complete, loss-free test evidence");
  check(count(events, EventType::SessionStarted) == 1 &&
        count(events, EventType::SessionStopped) == 1, "session terminals exactly once");
}
void pipeline_partial_and_empty_input_do_not_delegate() {
  SessionExecutor executor(1); Pipeline p("partial", executor);
  ConversationSession s("partial", p.engine(), &p.events);
  s.start(); s.push_audio(frame(1, false)); s.push_audio(frame(0)); p.input.wait(2);
  s.stop(); verify_trace(s);
  const auto t = s.timeline().snapshot();
  check(count(t, EventType::TranscriptPartial) == 1 && count(t, EventType::TranscriptFinal) == 1,
        "input observations retained");
  check(p.model.calls == 0 && p.speech.calls == 0 && p.audio.size() == 0 &&
        count(t, EventType::TurnStarted) == 0, "no task or audio from partial/empty input");
}
void pipeline_two_turns_have_separate_identity() {
  SessionExecutor executor(1); Pipeline p("two", executor);
  ConversationSession s("two", p.engine(), &p.events);
  MetricsRegistry metrics; RuntimeObserver observer(s.timeline(), metrics);
  s.start(); s.push_audio(frame(1)); p.captured.wait(EventType::ConversationTurnCompleted);
  s.push_audio(frame(2)); p.captured.wait(EventType::ConversationTurnCompleted, 2);
  s.stop(); verify_trace(s);
  const auto t = s.timeline().snapshot(); std::set<std::string> turns;
  for (const auto& e : t) if (e.type == EventType::TurnStarted) turns.insert(e.turn_id);
  check(turns == std::set<std::string>{"1", "2"} && p.audio.size() == 2 &&
        p.agent.history().size() == 5, "separate turns share only intended history");
  for (auto type : {EventType::AsrEndOfUtterance, EventType::TurnCompleted,
                   EventType::FirstAudio, EventType::ConversationTurnCompleted})
    check(count(t, type) == 2, "per-turn endpoint exactly once");
  check(metrics.histogram("byteturn_conversation_turn_duration_ms").count == 2 &&
        observer.pending_spans() == 0, "two completed metric spans, no leaked state");
}
void pipeline_model_failure_then_recovery() {
  SessionExecutor executor(1); Pipeline p("recovery", executor);
  p.model.action = [](const auto& history) -> LlmTurn {
    if (history.back().content == "1") throw std::runtime_error("fixture model failure");
    return {"The recovered answer is ready!", {}, true};
  };
  ConversationSession s("recovery", p.engine(), &p.events);
  s.start(); s.push_audio(frame(1)); p.captured.wait(EventType::ConversationTurnFailed);
  s.push_audio(frame(2)); p.captured.wait(EventType::ConversationTurnCompleted);
  s.stop(); verify_trace(s); const auto t = s.timeline().snapshot();
  check(count(t, EventType::TurnFailed) == 1 && count(t, EventType::TurnCompleted) == 1 &&
        count(t, EventType::ConversationTurnFailed) == 1 && p.audio.size() == 1, "failure and recovery isolated");
  check(p.agent.history().size() == 3 && p.agent.history()[1].content == "2", "failed history rolled back");
}
void pipeline_tts_failure_then_recovery() {
  SessionExecutor executor(1); Pipeline p("speech-recovery", executor); p.speech.fail_first = true;
  ConversationSession s("speech-recovery", p.engine(), &p.events);
  s.start(); s.push_audio(frame(1)); p.captured.wait(EventType::ConversationTurnFailed);
  s.push_audio(frame(2)); p.captured.wait(EventType::ConversationTurnCompleted);
  s.stop(); verify_trace(s); const auto t = s.timeline().snapshot();
  check(count(t, EventType::TurnCompleted) == 2 && count(t, EventType::ConversationTurnFailed) == 1 &&
        count(t, EventType::FirstAudio) == 1 && p.audio.size() == 1, "agent success is distinct from failed audio");
  check(index(t, EventType::ConversationTurnFailed) < index(t, EventType::FirstAudio), "recovered speech follows failed utterance drain");
}
void pipeline_wrong_session_fails_before_work() {
  SessionExecutor executor(1); Pipeline p("owner", executor);
  ConversationSession s("other", p.engine(), &p.events);
  expect_throw<std::invalid_argument>([&] { s.start(); }); s.stop();
  check(s.lifecycle() == SessionLifecycle::Failed && p.model.calls == 0 && p.speech.calls == 0,
        "mismatched ownership fails without starting work");
  check(count(s.timeline().snapshot(), EventType::SessionFailed) == 1, "startup failure observable");
}
void managed_pipelines_are_isolated_on_shared_executor() {
  SessionExecutor executor(2);
  auto a = std::make_shared<Pipeline>("a", executor), b = std::make_shared<Pipeline>("b", executor);
  Signal entered, release;
  a->model.action = [&](const auto&) -> LlmTurn { entered.set(); release.wait(); return {"Stale A answer!", {}, true}; };
  RuntimeManager runtime;
  const auto factory = [](std::shared_ptr<Pipeline> p) {
    return [p](const SessionHandle&) { SessionResources r; r.dependencies = p; r.engine = p->engine(); return r; };
  };
  auto admission_a = runtime.add("a", factory(a)), admission_b = runtime.add("b", factory(b));
  Finally cleanup([&] { release.set(); runtime.shutdown(); });
  check(await(admission_a.started) == SessionOutcome::Started && await(admission_b.started) == SessionOutcome::Started,
        "both pipelines started");
  runtime.push_audio(admission_a.handle, frame(1)); entered.wait();
  runtime.push_audio(admission_b.handle, frame(2)); b->audio.wait(1);
  // A FIFO sentinel proves B's entire response drained, rather than mistaking
  // first audio or a short sleep for completion.
  auto drained_b = executor.submit("b", [](const CancellationToken&, const std::string&) { return "drained"; });
  check(await(drained_b.result) == "drained", "other session completes while A is blocked");
  check(runtime.cancel_response(admission_a.handle), "cancel only A");
  release.set();
  auto drained_a = executor.submit("a", [](const CancellationToken&, const std::string&) { return "drained"; });
  await(drained_a.result);
  check(b->speech.cancels == 0 && b->agent.history().size() == 3 &&
        a->agent.history().size() == 1 && a->audio.size() == 0, "cancellation/history/audio isolated");
  runtime.remove(admission_a.handle); runtime.remove(admission_b.handle);
  check(await(admission_a.retired) == SessionOutcome::Removed && await(admission_b.retired) == SessionOutcome::Removed,
        "both resources retire normally");
  check(runtime.stats().sessions == 0 && runtime.stats().retired == 2 && runtime.stats().failures == 0,
        "no slot/resource leak");
}

// Native integration exercises the real FullDuplexConversation with a local
// protocol fixture. It is not a claim about a vendor SDK or acoustic VAD model.
struct NativeState {
  std::mutex mutex;
  std::condition_variable cv;
  int pushes = 0, commits = 0, closes = 0, cancels = 0;
  std::string cancelled_id;
  std::uint64_t played = 0;
  int rate = 0;
  bool accept = true;
  std::function<void()> on_push;
  RealtimeSpeechProvider::EventSink sink;
  void emit(RealtimeEvent e) { sink(std::move(e)); }  // Called only by the test thread.
  void wait_pushes(int n) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return pushes >= n; }), "native upload timed out");
  }
  void wait_commits(int n) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return commits >= n; }), "native commit timed out");
  }
};
class NativeSession final : public RealtimeSpeechSession {
 public:
  explicit NativeSession(std::shared_ptr<NativeState> state) : s(std::move(state)) {}
  bool push_audio(const AudioFrame&) override {
    if (s->on_push) s->on_push();
    std::lock_guard<std::mutex> lock(s->mutex); ++s->pushes; s->cv.notify_all(); return s->accept;
  }
  void commit_input() override { std::lock_guard<std::mutex> lock(s->mutex); ++s->commits; s->cv.notify_all(); }
  void cancel_response(const std::string& id, std::uint64_t samples, int hz) override {
    std::lock_guard<std::mutex> lock(s->mutex); ++s->cancels; s->cancelled_id = id; s->played = samples; s->rate = hz;
  }
  void close() override { std::lock_guard<std::mutex> lock(s->mutex); ++s->closes; }
 private:
  std::shared_ptr<NativeState> s;
};
class NativeProvider final : public RealtimeSpeechProvider {
 public:
  std::shared_ptr<NativeState> state = std::make_shared<NativeState>();
  int connects = 0;
  bool null_session = false;
  RealtimeSessionConfig config;
  std::unique_ptr<RealtimeSpeechSession> connect(const RealtimeSessionConfig& c, EventSink sink) override {
    ++connects; config = c;
    if (null_session) return {};
    state->sink = std::move(sink); state->emit({RealtimeEventType::Connected, {}, {}, {}, {}});
    return std::make_unique<NativeSession>(state);
  }
};
void emit_audio(NativeProvider& p, const std::string& id, int n = 4) {
  RealtimeEvent e; e.type = RealtimeEventType::AudioDelta; e.response_id = id;
  e.audio = {std::vector<std::int16_t>(static_cast<std::size_t>(n), 1), 24000, 1, false}; p.state->emit(std::move(e));
}
void native_configuration_validation() {
  NativeProvider p;
  for (int field = 0; field < 5; ++field) {
    FullDuplexConfig c; std::string id = "native";
    FullDuplexConversation::AudioSink sink = [](const DuplexAudio&) { return true; };
    if (field == 0) id.clear();
    if (field == 1) sink = {};
    if (field == 2) c.max_input_frames = 0;
    if (field == 3) c.input_sample_rate_hz = 0;
    if (field == 4) c.output_sample_rate_hz = 0;
    expect_throw<std::invalid_argument>([&] { FullDuplexConversation s(p, id, {}, sink, nullptr, c); });
  }
  check(p.connects == 0, "invalid config does not open provider"); p.null_session = true;
  expect_throw([&] { FullDuplexConversation s(p, "native", {}, [](const DuplexAudio&) { return true; }); });
}
void native_server_and_client_commit_ownership() {
  for (bool server : {true, false}) {
    NativeProvider p; FullDuplexConfig c; c.server_vad = server;
    FullDuplexConversation s(p, "native", {}, [](const DuplexAudio&) { return true; }, nullptr, c);
    p.state->emit({RealtimeEventType::InputSpeechStarted, {}, {}, {}, {}});
    s.push_audio(frame(1)); p.state->wait_pushes(1);
    if (server) p.state->emit({RealtimeEventType::InputSpeechEnded, {}, {}, {}, {}});
    else p.state->wait_commits(1);
    s.close();
    check(p.state->commits == (server ? 0 : 1) && p.state->closes == 1 &&
          p.config.server_vad == server, "exactly one commit authority");
  }
}
void native_bounded_input_and_provider_rejection() {
  NativeProvider p; Signal entered, release;
  p.state->accept = false;
  p.state->on_push = [&] { entered.set(); release.wait(); };
  EventBus bus; EventCapture captured; bus.subscribe([&](const Event& e) { captured.push(e); });
  FullDuplexConfig c; c.max_input_frames = 1;
  FullDuplexConversation s(p, "native", {}, [](const DuplexAudio&) { return true; }, &bus, c);
  Finally cleanup([&] { release.set(); });
  check(s.push_audio(frame(1)), "first frame queued"); entered.wait();
  check(s.push_audio(frame(2)) && !s.push_audio(frame(3)), "bounded input rejects newest frame");
  release.set(); p.state->wait_pushes(2); captured.wait(EventType::Error, 3); s.close();
  auto t = captured.snapshot(); int overload = 0, rejected = 0;
  for (const auto& e : t) { if (e.name == "audio_overload") ++overload; if (e.name == "provider_backpressure") ++rejected; }
  check(overload == 1 && rejected == 2 && p.state->pushes == 2, "distinct overload/rejection evidence");
}
void native_playback_ack_clamped_and_stale_packets_dropped() {
  NativeProvider p; std::vector<DuplexAudio> output; EventBus bus; EventCapture captured;
  bus.subscribe([&](const Event& e) { captured.push(e); });
  FullDuplexConversation s(p, "native", {}, [&](const DuplexAudio& a) { output.push_back(a); return true; }, &bus);
  p.state->emit({RealtimeEventType::ResponseStarted, "r1", {}, {}, {}});
  emit_audio(p, "r1"); emit_audio(p, "r1");
  s.playback_started("r1"); s.playback_started("r1");
  s.acknowledge_playback("wrong", 100); s.acknowledge_playback("r1", 3);
  s.acknowledge_playback("r1", 1); s.acknowledge_playback("r1", 100);
  p.state->emit({RealtimeEventType::InputSpeechStarted, {}, {}, {}, {}});
  p.state->emit({RealtimeEventType::InputSpeechStarted, {}, {}, {}, {}});
  emit_audio(p, "r1");
  p.state->emit({RealtimeEventType::ResponseStarted, "r2", {}, {}, {}});
  emit_audio(p, "r1"); emit_audio(p, "r2", 2); s.close();
  check(output.size() == 3 && output[0].start_sample == 0 && output[1].start_sample == 4 &&
        output[2].response_id == "r2" && output[2].start_sample == 0, "response-scoped offsets and stale fencing");
  check(p.state->cancels == 1 && p.state->cancelled_id == "r1" && p.state->played == 8 && p.state->rate == 24000,
        "provider receives bounded, monotonic audible offset once");
  const auto t = captured.snapshot();
  check(count(t, EventType::PlaybackStarted) == 1 && count(t, EventType::PlaybackProgress) == 2 &&
        count(t, EventType::BargeInDetected) == 1 && count(t, EventType::OutputCancelled) == 1,
        "duplicate/stale acknowledgement has no false endpoint");
}
void native_completed_generation_still_has_unheard_playback() {
  NativeProvider p; EventBus bus; EventCapture captured; int delivered = 0;
  bus.subscribe([&](const Event& e) { captured.push(e); });
  FullDuplexConversation s(p, "native", {}, [&](const DuplexAudio&) { ++delivered; return true; }, &bus);
  p.state->emit({RealtimeEventType::ResponseStarted, "r", {}, {}, {}}); emit_audio(p, "r", 8);
  p.state->emit({RealtimeEventType::ResponseCompleted, "r", {}, {}, {}});
  p.state->emit({RealtimeEventType::ResponseCompleted, "r", {}, {}, {}});
  s.playback_started("r"); s.acknowledge_playback("r", 3); emit_audio(p, "r");
  s.input_speech_started(); s.close();
  check(p.state->cancels == 1 && p.state->played == 3 && delivered == 1,
        "generation completion must not lose audible-offset cancellation");
  auto t = captured.snapshot();
  check(count(t, EventType::SpeechCompleted) == 1 && count(t, EventType::PlaybackStarted) == 1 &&
        count(t, EventType::OutputCancelled) == 1, "generation and playback endpoints remain distinct");
}
void native_fully_played_response_does_not_cancel_again() {
  NativeProvider p;
  FullDuplexConversation s(p, "native", {}, [](const DuplexAudio&) { return true; });
  p.state->emit({RealtimeEventType::ResponseStarted, "r", {}, {}, {}}); emit_audio(p, "r");
  s.playback_started("r"); s.acknowledge_playback("r", 4);
  p.state->emit({RealtimeEventType::ResponseCompleted, "r", {}, {}, {}});
  s.input_speech_started(); s.close();
  check(p.state->cancels == 0, "no cancellation for fully generated and heard response");
}
void native_rejected_audio_and_idempotent_close() {
  NativeProvider p; EventBus bus; EventCapture captured; int attempts = 0;
  bus.subscribe([&](const Event& e) { captured.push(e); });
  {
    FullDuplexConversation s(p, "native", {}, [&](const DuplexAudio&) { ++attempts; return false; }, &bus);
    p.state->emit({RealtimeEventType::ResponseStarted, "r", {}, {}, {}});
    emit_audio(p, "r"); emit_audio(p, "r"); s.close(); s.close();
    check(!s.push_audio(frame(1)), "closed ingress rejects audio");
    p.state->emit({RealtimeEventType::Error, {}, "late error", {}, "fixture"});
  }
  const auto t = captured.snapshot();
  check(attempts == 1 && p.state->cancels == 1 && p.state->played == 0 && p.state->closes == 1,
        "sink rejection cancels once at unheard offset");
  check(count(t, EventType::AudioOutput) == 0 && count(t, EventType::RealtimeSessionClosed) == 1 &&
        count(t, EventType::Error) == 0, "rejected audio and late errors not presented as delivered");
}
}  // namespace
int main(int argc, char** argv) {
  return run(argc, argv, {
    {"pipeline_partial_and_empty_input_do_not_delegate", pipeline_partial_and_empty_input_do_not_delegate},
    {"pipeline_two_turns_have_separate_identity", pipeline_two_turns_have_separate_identity},
    {"pipeline_model_failure_then_recovery", pipeline_model_failure_then_recovery},
    {"pipeline_tts_failure_then_recovery", pipeline_tts_failure_then_recovery},
    {"pipeline_wrong_session_fails_before_work", pipeline_wrong_session_fails_before_work},
    {"managed_pipelines_are_isolated_on_shared_executor", managed_pipelines_are_isolated_on_shared_executor},
    {"native_configuration_validation", native_configuration_validation},
    {"native_server_and_client_commit_ownership", native_server_and_client_commit_ownership},
    {"native_bounded_input_and_provider_rejection", native_bounded_input_and_provider_rejection},
    {"native_playback_ack_clamped_and_stale_packets_dropped", native_playback_ack_clamped_and_stale_packets_dropped},
    {"native_completed_generation_still_has_unheard_playback", native_completed_generation_still_has_unheard_playback},
    {"native_fully_played_response_does_not_cancel_again", native_fully_played_response_does_not_cancel_again},
    {"native_rejected_audio_and_idempotent_close", native_rejected_audio_and_idempotent_close}
  });
}
