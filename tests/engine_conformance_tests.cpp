#include "byteturn/native_duplex_conversation_engine.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/runtime_manager.h"
#include "test_support.h"

#include <atomic>
#include <thread>

using namespace byteturn;
using namespace realflow_test;
namespace {
AudioFrame pcm(int n = 4) { return {std::vector<std::int16_t>(n, 0), 24000, 1, false}; }
struct Native : RealtimeSpeechProvider {
  std::mutex mutex;
  std::condition_variable cv;
  EventSink sink;
  int active = 0, pushes = 0, cancels = 0, closes = 0;
  bool closed = false;
  std::string cancelled;
  std::uint64_t offset = 0;
  int rate = 0;
  Signal closing;
  void emit(RealtimeEventType type, std::string id = {}, AudioFrame frame = {}) {
    EventSink callback;
    { std::lock_guard<std::mutex> lock(mutex);
      if (closed) return;
      ++active; callback = sink; }
    Finally done([&] { std::lock_guard<std::mutex> lock(mutex); --active; cv.notify_all(); });
    callback({type, std::move(id), {}, std::move(frame), {}});
  }
  void wait_input(int n) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return pushes >= n; }), "native input drains");
  }
  struct Connection : RealtimeSpeechSession {
    Native& p;
    explicit Connection(Native& owner) : p(owner) {}
    bool push_audio(const AudioFrame&) override {
      std::lock_guard<std::mutex> lock(p.mutex); ++p.pushes; p.cv.notify_all(); return true;
    }
    void commit_input() override {}
    void cancel_response(const std::string& id, std::uint64_t samples, int hz) override {
      std::lock_guard<std::mutex> lock(p.mutex);
      ++p.cancels; p.cancelled = id; p.offset = samples; p.rate = hz;
    }
    void close() override {
      std::unique_lock<std::mutex> lock(p.mutex);
      p.closed = true; ++p.closes; p.closing.set();
      check(p.cv.wait_for(lock, 5s, [&] { return p.active == 0; }), "provider callback drain");
      p.sink = {};
    }
  };
  std::unique_ptr<RealtimeSpeechSession> connect(const RealtimeSessionConfig&, EventSink callback) override {
    sink = std::move(callback); emit(RealtimeEventType::Connected);
    return std::make_unique<Connection>(*this);
  }
};
struct Input : AsrProvider {
  std::mutex mutex; std::condition_variable cv; int pushes = 0;
  void reset() override {}
  void push(const AudioFrame& f, const std::function<void(std::string, bool)>& sink) override {
    if (f.end_of_utterance) sink("hello", true);
    std::lock_guard<std::mutex> lock(mutex); ++pushes; cv.notify_all();
  }
  void wait_input(int n) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return pushes >= n; }), "pipeline input drains");
  }
};
struct Model : LlmProvider {
  Signal release;
  LlmTurn complete(const std::vector<Message>&) override { return {}; }
  LlmTurn stream(const std::vector<Message>&, const TextDeltaSink& sink,
                 const std::function<bool()>&) override {
    sink("Hello there!"); release.wait(); return {"Hello there!", {}, true};
  }
};
struct Voice : TtsProvider {
  std::atomic<int> cancels{0};
  void cancel() override { ++cancels; }
  void synthesize(const std::string&, const std::function<bool(const AudioFrame&)>& sink) override { sink(pcm()); }
};
struct Fixture {
  bool native;
  Native provider;
  Input input; Model model; Voice voice; ToolRegistry tools;
  Agent agent{model, tools}; SessionExecutor executor{1};
  AsyncSession legacy{"test", agent, executor};
  Signal audio;
  std::atomic<int> outputs{0};
  explicit Fixture(bool n) : native(n) {}
  ~Fixture() { model.release.set(); }
  std::unique_ptr<ConversationEngine> engine() {
    if (native) return std::make_unique<NativeDuplexConversationEngine>(provider,
        FullDuplexConversation::TranscriptSink{}, [&](const DuplexAudio&) {
          ++outputs; audio.set(); return true;
        });
    return std::make_unique<PipelineConversationEngine>(input, legacy, voice,
        [](const std::string&, bool) {}, [&](const AudioFrame&) { ++outputs; audio.set(); });
  }
  void respond(ConversationSession& s) {
    if (native) {
      provider.emit(RealtimeEventType::ResponseStarted, "r");
      provider.emit(RealtimeEventType::AudioDelta, "r", pcm());
    } else {
      auto f = pcm(); f.end_of_utterance = true; check(s.push_audio(f), "trigger input");
    }
    audio.wait();
  }
};
void shared_session_contract() {
  for (bool native : {false, true}) {
    Fixture f(native); ConversationSession s("test", f.engine());
    Finally cleanup([&] { f.model.release.set(); s.stop(); });
    check(!s.playback_started("r") && !s.acknowledge_playback("r", 1), "created rejects player reports");
    s.start(); f.respond(s);
    check(s.capabilities().playback_acknowledgement == native, "capability matches support");
    check(s.playback_started("r") == native && s.acknowledge_playback("r", 2) == native,
          "shared explicit playback contract, pipeline unsupported");
    check(s.push_audio(pcm()), "continuous silence admitted during output");
    if (native) f.provider.wait_input(1); else f.input.wait_input(2);
    check(f.provider.cancels == 0 && f.voice.cancels == 0, "silence never cancels");
    check(s.state().agent_speaking, "output remains active while input drains");
    s.handle_event({EventType::InputSpeechStarted});
    s.handle_event({EventType::PlaybackProgress, {}, "r", 0, {}, {}, "4"});
    check(f.provider.cancels == 0 && f.voice.cancels == 0, "legacy hook has no control effects");
    check(s.cancel_response(), "explicit cancellation supported");
    if (native) {
      check(f.provider.offset == 2, "legacy playback observation does not advance offset");
      f.provider.emit(RealtimeEventType::AudioDelta, "r", pcm());
      check(f.outputs == 1, "post-cancel late audio fenced");
    }
    check(s.push_audio(pcm()), "input survives cancellation");
    if (native) f.provider.wait_input(2); else f.input.wait_input(3);
    f.model.release.set(); s.stop();
    check(!s.playback_started("r") && !s.acknowledge_playback("r", 4) && !s.cancel_response(),
          "stopped control admission closed");
    const auto stats = s.timeline().stats();
    check(stats.evicted == 0 && stats.dropped_notifications == 0 && stats.rejected == 0,
          "complete canonical trace");
  }
}
void native_playback_and_single_ingress() {
  Native p; EventBus bus; EventCapture captured; std::vector<DuplexAudio> audio;
  bus.subscribe([&](const Event& e) { captured.push(e); });
  ConversationSession s("test", std::make_unique<NativeDuplexConversationEngine>(p,
      FullDuplexConversation::TranscriptSink{}, [&](const DuplexAudio& a) { audio.push_back(a); return true; }), &bus);
  s.start();
  p.emit(RealtimeEventType::ResponseStarted, "r");
  p.emit(RealtimeEventType::AudioDelta, "r", pcm(4));
  p.emit(RealtimeEventType::AudioDelta, "r", pcm(4));
  p.emit(RealtimeEventType::ResponseCompleted, "r");
  check(!s.playback_started("old") && !s.acknowledge_playback("old", 7), "response identity fenced");
  check(s.playback_started("r") && s.playback_started("r"), "duplicate start admitted without duplicate event");
  check(s.acknowledge_playback("r", 3) && s.acknowledge_playback("r", 1), "monotonic progress");
  p.emit(RealtimeEventType::InputSpeechStarted);
  check(p.cancels == 1 && p.cancelled == "r" && p.offset == 3 && p.rate == 24000,
        "barge-in truncates completed generation to actually heard samples");
  check(!s.acknowledge_playback("r", 8), "cancelled response rejects ack");
  p.emit(RealtimeEventType::AudioDelta, "r", pcm());
  p.emit(RealtimeEventType::ResponseCompleted, "r");
  p.emit(RealtimeEventType::ResponseStarted, "next");
  p.emit(RealtimeEventType::AudioDelta, "r", pcm());
  p.emit(RealtimeEventType::AudioDelta, "next", pcm(4));
  check(!s.playback_started("r") && !s.acknowledge_playback("r", 999), "old identity cannot affect next response");
  check(s.acknowledge_playback("next", 999), "progress clamped to delivered samples");
  check(s.cancel_response(), "explicit cancel next");
  check(p.cancels == 2 && p.offset == 4, "clamped cancellation offset");
  s.stop(); check(s.timeline().flush(), "notifications drained");
  auto trace = s.timeline().snapshot(), delivered = captured.snapshot();
  check(trace.size() == delivered.size(), "one canonical delivery per native event");
  for (std::size_t i = 0; i < trace.size(); ++i) {
    check(trace[i].sequence == i + 1 && delivered[i].sequence == trace[i].sequence &&
          trace[i].session_id == "test" && trace[i].generation != 0 &&
          trace[i].received_at != std::chrono::steady_clock::time_point{}, "canonical identity and order");
  }
  check(audio.size() == 3 && audio[0].start_sample == 0 && audio[1].start_sample == 4 &&
        audio[2].response_id == "next" && audio[2].start_sample == 0,
        "late audio fenced and next response resets offset");
  check(count(trace, EventType::PlaybackStarted) == 1 && count(trace, EventType::PlaybackProgress) == 2 &&
        count(trace, EventType::AudioOutput) == 3 && count(trace, EventType::OutputCancelled) == 2 &&
        count(trace, EventType::BargeInDetected) == 1 && count(trace, EventType::ResponseCancelRequested) == 1 &&
        count(trace, EventType::SpeechCompleted) == 1 && count(trace, EventType::RealtimeSessionClosed) == 1,
        "exact event counts distinguish barge-in and explicit cancellation");
  check(index(trace, EventType::PlaybackProgress) < index(trace, EventType::BargeInDetected) &&
        index(trace, EventType::BargeInDetected) < index(trace, EventType::OutputCancelled), "causal cancellation order");
  check(s.timeline().stats().evicted == 0 && s.timeline().stats().dropped_notifications == 0,
        "trace is complete");
}
void shared_runtime_fencing() {
  for (bool native : {false, true}) {
    RuntimeManager runtime;
    auto first = std::make_shared<Fixture>(native);
    auto factory = [](std::shared_ptr<Fixture> f) {
      return [f](const SessionHandle&) { SessionResources r; r.dependencies = f; r.engine = f->engine(); return r; };
    };
    auto old = runtime.add("test", factory(first)); check(await(old.started) == SessionOutcome::Started, "first startup");
    runtime.remove(old.handle); check(await(old.retired) == SessionOutcome::Removed, "first retired");
    auto second = std::make_shared<Fixture>(native);
    auto current = runtime.add("test", factory(second)); check(await(current.started) == SessionOutcome::Started, "replacement startup");
    if (native) {
      second->provider.emit(RealtimeEventType::ResponseStarted, "r");
      second->provider.emit(RealtimeEventType::AudioDelta, "r", pcm());
    }
    check(!runtime.playback_started(old.handle, "r") && !runtime.acknowledge_playback(old.handle, "r", 4) &&
          !runtime.cancel_response(old.handle), "retired handle cannot control replacement");
    auto foreign = current.handle; ++foreign.runtime_id;
    check(!runtime.acknowledge_playback(foreign, "r", 4), "foreign runtime rejected");
    check(runtime.playback_started(current.handle, "r") == native &&
          runtime.acknowledge_playback(current.handle, "r", 2) == native, "runtime forwards support result");
    if (native) {
      runtime.cancel_response(current.handle);
      check(second->provider.offset == 2 && second->provider.cancels == 1, "stale report caused no mutation");
    }
    runtime.shutdown(); check(await(current.retired) == SessionOutcome::RuntimeShutdown, "shutdown retirement");
    check(runtime.stats().sessions == 0 && !runtime.acknowledge_playback(current.handle, "r", 4), "shutdown admission closed");
  }
}
void native_shutdown_quiescence() {
  auto p = std::make_shared<Native>(); Signal entered, release;
  RuntimeManager runtime;
  auto a = runtime.add("test", [&](const SessionHandle&) {
    SessionResources r; r.dependencies = p;
    r.engine = std::make_unique<NativeDuplexConversationEngine>(*p,
        FullDuplexConversation::TranscriptSink{}, [&](const DuplexAudio&) {
          entered.set(); release.wait(); return true;
        }); return r;
  });
  check(await(a.started) == SessionOutcome::Started, "startup");
  p->emit(RealtimeEventType::ResponseStarted, "r");
  auto callback = std::async(std::launch::async, [&] { p->emit(RealtimeEventType::AudioDelta, "r", pcm()); });
  Finally cleanup([&] { release.set(); }); entered.wait();
  runtime.request_shutdown(); p->closing.wait();
  check(a.retired.wait_for(0s) != std::future_status::ready, "retirement waits for in-flight callback");
  check(!runtime.playback_started(a.handle, "r"), "shutdown rejects new controls");
  release.set(); await(callback);
  check(runtime.wait_shutdown(5s) && await(a.retired) == SessionOutcome::RuntimeShutdown && p->closes == 1,
        "provider quiesced before retirement");
}
}  // namespace
int main(int argc, char** argv) {
  return run(argc, argv, {
    {"shared_session_contract", shared_session_contract},
    {"native_playback_and_single_ingress", native_playback_and_single_ingress},
    {"shared_runtime_fencing", shared_runtime_fencing},
    {"native_shutdown_quiescence", native_shutdown_quiescence}
  });
}
