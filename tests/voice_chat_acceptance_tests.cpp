#include "voice_chat_adapter.h"
#include "byteturn/mock_providers.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "test_support.h"

#include <atomic>
#include <thread>

using namespace realflow_app;
using namespace realflow_test;
namespace {
const AuthenticatedClient alice{"alice", "connection-a"};
const AuthenticatedClient bob{"bob", "connection-b"};
const AudioFrame input{{7, 8}, 16000, 1, false};
const AudioFrame eou{{}, 16000, 1, true};

bool same(const SessionHandle& a, const SessionHandle& b) {
  return a.id == b.id && a.runtime_id == b.runtime_id && a.generation == b.generation;
}
struct Probe {
  std::mutex mutex;
  std::vector<std::vector<Message>> histories;
  MediaSinks late;
  EventTimeline::Sink late_event;
  std::vector<AudioFrame> inputs;
  Signal start_entered, start_release, stop_entered, stop_release, model_entered;
  bool gate_start = false, gate_stop = false, fail_start = false, fail_push = false;
  std::atomic<bool> hold_model{false};
  std::atomic<int> cancelled{0}, constructed{0}, destroyed{0}, engine_stops{0};
};
// Audit the history passed to the existing mock; do not implement another model.
class AuditedLlm final : public LlmProvider {
 public:
  AuditedLlm(std::shared_ptr<Probe> probe, std::string label)
      : probe_(std::move(probe)), mock_({{{label + " reply."}, {}},
                                      {{label + " reply two."}, {}}}) {}
  LlmTurn complete(const std::vector<Message>& h) override { return mock_.complete(h); }
  LlmTurn stream(const std::vector<Message>& h, const TextDeltaSink& sink,
                 const std::function<bool()>& cancelled) override {
    {
      std::lock_guard<std::mutex> lock(probe_->mutex);
      probe_->histories.push_back(h);
    }
    probe_->model_entered.set();
    const auto deadline = VoiceChatConnection::Clock::now() + 5s;
    while (probe_->hold_model.load() && !cancelled()) {
      if (VoiceChatConnection::Clock::now() >= deadline)
        throw std::runtime_error("model gate timed out");
      std::this_thread::yield();
    }
    if (cancelled()) ++probe_->cancelled;
    return mock_.stream(h, sink, cancelled);
  }
 private:
  std::shared_ptr<Probe> probe_;
  MockLlmProvider mock_;
};
class AuditedAsr final : public AsrProvider {
 public:
  AuditedAsr(std::shared_ptr<Probe> p, const std::string& label)
      : p_(std::move(p)), mock_({{{label + " partial"}, label + " first"},
                                 {{}, label + " second"}}) {}
  void reset() override { mock_.reset(); }
  void push(const AudioFrame& frame,
            const std::function<void(std::string, bool)>& sink) override {
    { std::lock_guard<std::mutex> lock(p_->mutex); p_->inputs.push_back(frame); }
    mock_.push(frame, sink);
  }
 private:
  std::shared_ptr<Probe> p_;
  MockAsrProvider mock_;
};
struct Dependencies {
  std::shared_ptr<Probe> probe;
  AuditedAsr asr;
  AuditedLlm llm;
  MockTtsProvider tts;
  ToolRegistry tools;
  Agent agent;
  SessionExecutor executor{1};
  AsyncSession turns;
  Dependencies(const SessionHandle& h, std::shared_ptr<Probe> p,
               const std::string& label, std::int16_t sample)
      : probe(std::move(p)), asr(probe, label),
        llm(probe, label), tts({{{sample, sample}, 16000, 1, false}}),
        agent(llm, tools), turns(h.id, agent, executor) { ++probe->constructed; }
  ~Dependencies() { ++probe->destroyed; }
};
class ProbedEngine final : public ConversationEngine {
 public:
  ProbedEngine(std::unique_ptr<ConversationEngine> inner, std::shared_ptr<Probe> p)
      : inner_(std::move(inner)), p_(std::move(p)) {}
  void start(ConversationEngineContext context) override {
    { std::lock_guard<std::mutex> lock(p_->mutex); p_->late_event = context.emit; }
    p_->start_entered.set();
    if (p_->gate_start) p_->start_release.wait();
    if (p_->fail_start) throw std::runtime_error("scripted start failure");
    inner_->start(std::move(context));
  }
  bool push_audio(AudioFrame f) override {
    if (p_->fail_push) throw std::runtime_error("scripted engine failure");
    return inner_->push_audio(std::move(f));
  }
  bool cancel_response() override { return inner_->cancel_response(); }
  void handle_event(const Event& e) override { inner_->handle_event(e); }
  void stop() override {
    ++p_->engine_stops;
    p_->stop_entered.set();
    if (p_->gate_stop) p_->stop_release.wait();
    inner_->stop();
  }
  ConversationCapabilities capabilities() const override { return inner_->capabilities(); }
  ConversationStateSnapshot state() const override { return inner_->state(); }
 private:
  std::unique_ptr<ConversationEngine> inner_;
  std::shared_ptr<Probe> p_;
};
VoiceChatConnection::Factory factory(std::shared_ptr<Probe> p,
                                    std::string label, std::int16_t sample) {
  return [p, label, sample](const SessionHandle& h, MediaSinks sinks) {
    auto d = std::make_shared<Dependencies>(h, p, label, sample);
    {
      std::lock_guard<std::mutex> lock(p->mutex);
      p->late = sinks;
    }
    SessionResources result;
    result.dependencies = d;
    auto pipeline = std::make_unique<PipelineConversationEngine>(
        d->asr, d->turns, d->tts, std::move(sinks.transcript), std::move(sinks.audio));
    result.engine = std::make_unique<ProbedEngine>(std::move(pipeline), p);
    return result;
  };
}
SessionAdmission running(VoiceChatConnection& c, const AuthenticatedClient& who) {
  auto reply = c.start(who);
  check(reply.status.code == ChatCode::Ok && bool(reply.admission), "start accepted");
  check(await(reply.admission.started) == SessionOutcome::Started, "started outcome");
  check(c.status(who, reply.admission.handle).phase == ChatPhase::Running, "running status");
  return reply.admission;
}
std::vector<ChatOutput> turn(VoiceChatConnection& c, const AuthenticatedClient& who,
                            const SessionHandle& h) {
  auto distinct_input = input;
  distinct_input.samples = {static_cast<std::int16_t>(who.user_id == "bob" ? 22 : 11)};
  check(c.push_audio(who, h, distinct_input) == ChatCode::Ok, "audio accepted");
  check(c.push_audio(who, h, eou) == ChatCode::Ok, "EOU accepted");
  std::vector<ChatOutput> result;
  const auto deadline = VoiceChatConnection::Clock::now() + 5s;
  bool complete = false;
  while (!complete && VoiceChatConnection::Clock::now() < deadline) {
    for (auto& out : c.take_output(who, h)) {
      if (out.kind == ChatOutput::Kind::Observation &&
          out.event.type == EventType::ConversationTurnCompleted) complete = true;
      result.push_back(std::move(out));
    }
    std::this_thread::yield();
  }
  check(complete, "turn completion observation missing");
  check(c.status(who, h).dropped_output == 0, "complete outbound trace without overflow");
  return result;
}
void isolated(const std::vector<ChatOutput>& outputs, const SessionHandle& h,
              const std::string& label, std::int16_t sample) {
  bool audio = false, final = false;
  std::vector<Event> events;
  std::uint64_t last = 0;
  for (const auto& out : outputs) {
    check(same(out.handle, h), "outbound session identity");
    if (out.kind == ChatOutput::Kind::Audio) {
      audio = true;
      for (auto value : out.audio.samples) check(value == sample, "independent PCM");
    } else if (out.kind == ChatOutput::Kind::Transcript) {
      check(out.text.find(label) == 0, "independent transcript");
      final |= out.final;
    } else {
      check(out.event.session_id == h.id && out.event.generation == h.generation &&
            out.event.sequence > last, "canonical observation identity/order");
      last = out.event.sequence;
      events.push_back(out.event);
    }
  }
  check(audio && final, "audio and final transcript delivered");
  check(index(events, EventType::TranscriptFinal) < index(events, EventType::ModelStarted),
        "transcript precedes model");
  check(index(events, EventType::FirstAudio) < index(events, EventType::ConversationTurnCompleted),
        "audio precedes completion");
}
void retired(VoiceChatConnection& c, const AuthenticatedClient& who,
             const SessionAdmission& a, std::shared_ptr<Probe> p,
             SessionOutcome expected = SessionOutcome::Removed) {
  check(await(a.retired) == expected, "retirement reason");
  const auto status = c.status(who, a.handle);
  check(status.phase == ChatPhase::Retired && status.retirement == expected, "retirement status");
  check(status.flush_requested && !status.player_stopped, "retirement does not prove silence");
  check(p->destroyed == p->constructed && p->engine_stops > 0, "resource teardown confirmed");
}
struct Player {
  SessionHandle handle;
  std::vector<AudioFrame> buffered;
  bool stopped = false;
  void accept(const ChatOutput& packet) {
    if (!stopped && same(packet.handle, handle) && packet.kind == ChatOutput::Kind::Audio)
      buffered.push_back(packet.audio);
  }
  void flush(VoiceChatConnection& c, const AuthenticatedClient& who) {
    check(c.status(who, handle).flush_requested, "flush requested");
    stopped = true;
    buffered.clear();
    check(c.acknowledge_player_stop(who, handle) == ChatCode::Ok, "player stop acknowledgement");
  }
};

void two_clients() {
  RuntimeManager runtime;
  auto pa = std::make_shared<Probe>(), pb = std::make_shared<Probe>();
  VoiceChatConnection a(runtime, alice, factory(pa, "Alice", 11));
  VoiceChatConnection b(runtime, bob, factory(pb, "Bob", 22));
  auto aa = running(a, alice), bb = running(b, bob);
  auto at = std::async(std::launch::async, [&] { return turn(a, alice, aa.handle); });
  auto bt = std::async(std::launch::async, [&] { return turn(b, bob, bb.handle); });
  const auto ao = await(at), bo = await(bt);
  isolated(ao, aa.handle, "Alice", 11);
  isolated(bo, bb.handle, "Bob", 22);
  check(runtime.stats().sessions == 2, "two managed sessions");
  for (const auto& pair : {std::make_pair(pa, 11), std::make_pair(pb, 22)}) {
    std::lock_guard<std::mutex> lock(pair.first->mutex);
    check(pair.first->inputs.size() == 2 && pair.first->inputs[0].samples[0] == pair.second &&
          pair.first->inputs[1].end_of_utterance, "input routed only to owning ASR");
  }
  check(same(a.start(alice).admission.handle, aa.handle), "running start idempotent");
  Player player{aa.handle, {}, false};
  for (const auto& out : ao) player.accept(out);
  check(!player.buffered.empty(), "player has queued PCM");
  check(a.stop(alice, aa.handle) == ChatCode::Ok, "stop A");
  check(a.stop(alice, aa.handle) == ChatCode::Ok, "stop A idempotent");
  check(a.push_audio(alice, aa.handle, input) == ChatCode::NotRunning, "stop fences input");
  check(a.take_output(alice, aa.handle).empty(), "stop clears outbound");
  retired(a, alice, aa, pa);
  check(a.start(alice).status.code == ChatCode::AwaitingPlayer, "restart waits for player");
  player.flush(a, alice);
  for (const auto& out : ao) player.accept(out);
  check(player.buffered.empty(), "late transport packets do not restart playback");
  isolated(turn(b, bob, bb.handle), bb.handle, "Bob", 22);
  {
    std::lock_guard<std::mutex> lock(pb->mutex);
    check(pb->histories.size() == 2, "B retains independent history");
    bool prior = false;
    for (const auto& m : pb->histories.back()) {
      check(m.content.find("Alice") == std::string::npos, "no cross-user history");
      prior |= m.content == "Bob first";
    }
    check(prior, "B history survived A stop");
  }
  b.stop(bob, bb.handle);
  retired(b, bob, bb, pb);
  check(runtime.stats().sessions == 0 && runtime.stats().retired == 2, "no orphan sessions");
}
void authorization_and_stale() {
  RuntimeManager runtime;
  auto p = std::make_shared<Probe>(), q = std::make_shared<Probe>();
  VoiceChatConnection a(runtime, alice, factory(p, "Alice", 11));
  VoiceChatConnection b(runtime, bob, factory(q, "Bob", 22));
  auto aa = running(a, alice), bb = running(b, bob);
  const AuthenticatedClient other_device{"alice", "another-device"};
  for (const auto& intruder : {bob, other_device}) {
    check(a.start(intruder).status.code == ChatCode::Forbidden, "authorize start");
    check(a.push_audio(intruder, aa.handle, input) == ChatCode::Forbidden, "authorize audio");
    check(a.stop(intruder, aa.handle) == ChatCode::Forbidden, "authorize stop");
    auto hidden = a.status(intruder, aa.handle);
    check(hidden.code == ChatCode::Forbidden && hidden.handle.id.empty(), "status no disclosure");
    check(a.take_output(intruder, aa.handle).empty(), "authorize output");
    check(a.acknowledge_player_stop(intruder, aa.handle) == ChatCode::Forbidden, "authorize flush");
    check(a.heartbeat(intruder, VoiceChatConnection::Clock::now()) == ChatCode::Forbidden,
          "authorize heartbeat");
  }
  check(a.stop(alice, bb.handle) == ChatCode::StaleSession, "foreign handle rejected");
  check(a.push_audio(alice, bb.handle, input) == ChatCode::StaleSession, "foreign input rejected");
  check(a.status(alice, bb.handle).code == ChatCode::StaleSession, "foreign status rejected");
  MediaSinks old;
  EventTimeline::Sink old_event;
  { std::lock_guard<std::mutex> lock(p->mutex); old = p->late; old_event = p->late_event; }
  a.stop(alice, aa.handle);
  retired(a, alice, aa, p);
  a.acknowledge_player_stop(alice, aa.handle);
  auto replacement = running(a, alice);
  check(aa.handle.id == replacement.handle.id &&
        aa.handle.generation != replacement.handle.generation, "fresh runtime incarnation");
  old.audio({{99}, 16000, 1, false});
  old.transcript("stale", true);
  check(!old_event(Event{EventType::AudioOutput}), "old observation ingress closed");
  check(a.stop(alice, aa.handle) == ChatCode::StaleSession, "stale stop");
  check(a.push_audio(alice, aa.handle, input) == ChatCode::StaleSession, "stale input");
  check(a.acknowledge_player_stop(alice, aa.handle) == ChatCode::StaleSession, "stale flush ack");
  isolated(turn(a, alice, replacement.handle), replacement.handle, "Alice", 11);
  a.stop(alice, replacement.handle); await(replacement.retired);
  b.stop(bob, bb.handle); await(bb.retired);
}
void start_race_and_stop_during_start() {
  RuntimeManager runtime;
  auto p = std::make_shared<Probe>(); p->gate_start = true;
  VoiceChatConnection a(runtime, alice, factory(p, "Alice", 11));
  Finally release([&] { p->start_release.set(); });
  std::vector<std::future<StartReply>> calls;
  Signal go;
  for (int i = 0; i < 8; ++i) calls.push_back(std::async(std::launch::async, [&] {
    go.wait(); return a.start(alice);
  }));
  go.set();
  auto first = await(calls[0]).admission;
  for (std::size_t i = 1; i < calls.size(); ++i)
    check(same(await(calls[i]).admission.handle, first.handle), "racing start shared handle");
  p->start_entered.wait();
  check(p->constructed == 1 && runtime.stats().admitted == 1, "single startup allocation");
  check(a.status(alice, first.handle).phase == ChatPhase::Starting, "pending startup");
  check(a.push_audio(alice, first.handle, input) == ChatCode::NotRunning, "no early audio");
  a.stop(alice, first.handle);
  check(a.start(alice).status.code == ChatCode::Stopping, "no replacement while retiring");
  check(first.retired.wait_for(0ms) != std::future_status::ready, "startup still owns capacity");
  p->start_release.set();
  check(await(first.started) == SessionOutcome::Removed, "stop won startup race");
  retired(a, alice, first, p);
  check(runtime.stats().sessions == 0, "startup cleanup");
}
void capacity_and_queue() {
  RuntimeConfig config; config.max_sessions = 1;
  RuntimeManager runtime(config);
  auto p = std::make_shared<Probe>(), q = std::make_shared<Probe>(); p->gate_stop = true;
  VoiceChatConnection a(runtime, alice, factory(p, "Alice", 11));
  VoiceChatConnection b(runtime, bob, factory(q, "Bob", 22));
  Finally release([&] { p->stop_release.set(); });
  auto aa = running(a, alice);
  check(b.start(bob).admission.status == AdmissionStatus::CapacityReached, "full capacity");
  a.stop(alice, aa.handle); p->stop_entered.wait();
  check(b.start(bob).admission.status == AdmissionStatus::CapacityReached, "retiring counts");
  check(aa.retired.wait_for(0ms) != std::future_status::ready, "no early retirement");
  p->stop_release.set(); await(aa.retired);
  auto bb = running(b, bob); b.stop(bob, bb.handle); await(bb.retired);
  check(runtime.stats().sessions == 0, "capacity recovered");

  RuntimeConfig queued; queued.max_sessions = 3; queued.startup_workers = 1;
  queued.max_pending_starts = 1;
  RuntimeManager queue_runtime(queued);
  auto gate = std::make_shared<Probe>(); gate->gate_start = true;
  VoiceChatConnection x(queue_runtime, alice, factory(gate, "A", 1));
  VoiceChatConnection y(queue_runtime, bob, factory(q, "B", 2));
  VoiceChatConnection z(queue_runtime, {"third", "c"}, factory(q, "C", 3));
  Finally release_queue([&] { gate->start_release.set(); });
  auto xx = x.start(alice).admission; gate->start_entered.wait();
  auto yy = y.start(bob).admission;
  check(bool(yy), "queued reservation");
  check(z.start({"third", "c"}).admission.status == AdmissionStatus::StartupQueueFull,
        "queue full surfaced");
  y.stop(bob, yy.handle);
  check(await(yy.started) == SessionOutcome::Removed && await(yy.retired) == SessionOutcome::Removed,
        "queued stop retires without factory");
  x.stop(alice, xx.handle); gate->start_release.set(); await(xx.retired);
}
void failures_and_cancellation() {
  RuntimeManager runtime;
  auto bad = std::make_shared<Probe>(), good = std::make_shared<Probe>(); bad->fail_start = true;
  VoiceChatConnection a(runtime, alice, factory(bad, "Alice", 11));
  VoiceChatConnection b(runtime, bob, factory(good, "Bob", 22));
  auto bb = running(b, bob), aa = a.start(alice).admission;
  check(await(aa.started) == SessionOutcome::StartupFailed, "failed startup outcome");
  retired(a, alice, aa, bad, SessionOutcome::StartupFailed);
  isolated(turn(b, bob, bb.handle), bb.handle, "Bob", 22);
  auto fatal = std::make_shared<Probe>(); fatal->fail_push = true;
  VoiceChatConnection c(runtime, {"fatal", "c"}, factory(fatal, "Fatal", 33));
  auto cc = running(c, {"fatal", "c"});
  check(c.push_audio({"fatal", "c"}, cc.handle, input) == ChatCode::NotRunning, "engine failure");
  retired(c, {"fatal", "c"}, cc, fatal, SessionOutcome::EngineFailure);
  isolated(turn(b, bob, bb.handle), bb.handle, "Bob", 22);
  b.stop(bob, bb.handle); await(bb.retired);
  check(runtime.stats().failures == 2 && runtime.stats().sessions == 0, "failure accounting");

  auto held = std::make_shared<Probe>(); held->hold_model = true;
  VoiceChatConnection d(runtime, alice, factory(held, "Held", 44));
  auto dd = running(d, alice);
  d.push_audio(alice, dd.handle, eou); held->model_entered.wait();
  d.stop(alice, dd.handle);
  retired(d, alice, dd, held);
  check(held->cancelled == 1, "stop propagated model cancellation");
}
void disconnect_and_reconnect() {
  RuntimeManager runtime;
  auto p = std::make_shared<Probe>(), q = std::make_shared<Probe>();
  const auto zero = VoiceChatConnection::Clock::time_point{};
  ChatConfig config; config.liveness_timeout = 100ms;
  VoiceChatConnection a(runtime, alice, factory(p, "Alice", 11), config, zero);
  VoiceChatConnection b(runtime, bob, factory(q, "Bob", 22), config, zero);
  auto aa = running(a, alice), bb = running(b, bob);
  MediaSinks old;
  { std::lock_guard<std::mutex> lock(p->mutex); old = p->late; }
  a.disconnect(); a.disconnect();
  check(await(aa.retired) == SessionOutcome::Removed, "explicit disconnect cleanup");
  check(a.start(alice).status.code == ChatCode::Disconnected, "closed binding cannot restart");
  check(a.push_audio(alice, aa.handle, input) == ChatCode::Disconnected, "disconnected input");
  AuthenticatedClient new_client{"alice", "connection-a-new"};
  VoiceChatConnection reconnected(runtime, new_client, factory(p, "Alice", 11), config, zero);
  auto fresh = running(reconnected, new_client);
  check(!same(fresh.handle, aa.handle), "reconnect fresh identity");
  check(reconnected.start(alice).status.code == ChatCode::Forbidden, "old connection denied");
  check(reconnected.stop(new_client, aa.handle) == ChatCode::StaleSession, "old handle denied");
  old.audio({{99}, 16000, 1, false}); old.transcript("stale", true);
  isolated(turn(reconnected, new_client, fresh.handle), fresh.handle, "Alice", 11);
  check(b.heartbeat(bob, zero + 90ms) == ChatCode::Ok, "live B heartbeat");
  reconnected.tick(zero + 100ms); b.tick(zero + 100ms);
  check(await(fresh.retired) == SessionOutcome::Removed, "silent timeout cleanup");
  check(reconnected.heartbeat(new_client, zero + 101ms) == ChatCode::Disconnected,
        "heartbeat cannot revive expired binding");
  isolated(turn(b, bob, bb.handle), bb.handle, "Bob", 22);
  b.disconnect(); await(bb.retired);
  check(runtime.stats().sessions == 0, "disconnect no orphan sessions");
  // Dropping a transport binding also requests removal, without waiting in its destructor.
  SessionAdmission dropped;
  { VoiceChatConnection temporary(runtime, alice, factory(p, "A", 1)); dropped = running(temporary, alice); }
  check(await(dropped.retired) == SessionOutcome::Removed, "RAII disconnect");
}
void bounded_output() {
  RuntimeManager runtime;
  auto p = std::make_shared<Probe>();
  ChatConfig config; config.max_payload_bytes = 1024;
  VoiceChatConnection a(runtime, alice, factory(p, "Alice", 11), config);
  auto aa = running(a, alice);
  AudioFrame huge{std::vector<std::int16_t>(513), 16000, 1, false};
  check(a.push_audio(alice, aa.handle, huge) == ChatCode::InvalidAudio, "bounded input");
  MediaSinks sinks;
  { std::lock_guard<std::mutex> lock(p->mutex); sinks = p->late; }
  sinks.audio(huge);
  auto status = a.status(alice, aa.handle);
  check(status.dropped_output == 1 && status.flush_requested, "oversize output fails closed");
  check(await(aa.retired) == SessionOutcome::ReportedFailure, "output overflow retires");
  check(a.take_output(alice, aa.handle).empty(), "overflow clears partial media");
  a.acknowledge_player_stop(alice, aa.handle);
  auto next = running(a, alice);
  { std::lock_guard<std::mutex> lock(p->mutex); sinks = p->late; }
  for (std::size_t i = 0; i <= config.max_output_messages; ++i) sinks.transcript("fixture", false);
  check(a.status(alice, next.handle).dropped_output == 1, "bounded message count");
  check(await(next.retired) == SessionOutcome::ReportedFailure, "queue overflow retires");
}
}  // namespace
int main(int argc, char** argv) {
  return run(argc, argv, {
    {"two_clients_independent_pipeline_and_player_flush", two_clients},
    {"ownership_and_stale_callbacks", authorization_and_stale},
    {"racing_start_and_stop_during_startup", start_race_and_stop_during_start},
    {"capacity_and_startup_queue", capacity_and_queue},
    {"failure_isolation_and_cancellation", failures_and_cancellation},
    {"disconnect_timeout_and_reconnect", disconnect_and_reconnect},
    {"bounded_input_and_output", bounded_output},
  });
}
