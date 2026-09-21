#include "byteturn/runtime_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace byteturn;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* text) {
  if (!condition) throw std::runtime_error(text);
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_all();
    if (!cv.wait_for(lock, 5s, [&] { return released; }))
      throw std::runtime_error("gate timeout");
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return entered; }), "gate not entered");
  }
  void open() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true; cv.notify_all();
  }
  ~Gate() { open(); }
};
SessionOutcome await(const std::shared_future<SessionOutcome>& future) {
  check(future.valid() && future.wait_for(5s) == std::future_status::ready,
        "completion ticket timeout");
  return future.get();
}
RuntimeConfig limits(std::size_t sessions = 4) {
  RuntimeConfig c;
  c.max_sessions = sessions; c.max_pending_starts = sessions;
  c.startup_workers = 2; c.supervisor_interval = 1ms;
  return c;
}
struct Probe {
  std::atomic<int> starts{0}, stops{0}, audio{0}, destroyed{0};
  std::function<void(ConversationEngineContext)> on_start;
  std::function<void()> on_audio, on_stop, on_destroy;
};
class Engine final : public ConversationEngine {
 public:
  explicit Engine(std::shared_ptr<Probe> probe) : p(std::move(probe)) {}
  ~Engine() override { ++p->destroyed; if (p->on_destroy) p->on_destroy(); }
  void start(ConversationEngineContext ctx) override {
    ++p->starts; if (p->on_start) p->on_start(std::move(ctx));
  }
  bool push_audio(AudioFrame) override { ++p->audio; if (p->on_audio) p->on_audio(); return true; }
  void handle_event(const Event&) override { if (p->on_audio) p->on_audio(); }
  void stop() override { ++p->stops; if (p->on_stop) p->on_stop(); }
  ConversationCapabilities capabilities() const override { return {}; }
  ConversationStateSnapshot state() const override { return {}; }
 private:
  std::shared_ptr<Probe> p;
};
RuntimeManager::SessionFactory factory(std::shared_ptr<Probe> p = std::make_shared<Probe>()) {
  return [p](const SessionHandle&) {
    SessionResources r; r.engine = std::make_unique<Engine>(p); return r;
  };
}

void admission_and_retirement() {
  RuntimeManager r(limits()); auto p = std::make_shared<Probe>();
  auto a = r.add("a", factory(p)); check(bool(a), "admission");
  check(await(a.started) == SessionOutcome::Started, "startup");
  check(r.find(a.handle)->phase == SessionPhase::Running, "registry phase");
  check(r.push_audio(a.handle, {}), "input admitted");
  r.remove(a.handle); check(!r.push_audio(a.handle, {}), "removed input rejected");
  check(await(a.retired) == SessionOutcome::Removed, "retirement");
  check(!r.find(a.handle) && p->stops == 1 && p->destroyed == 1, "resources reaped");
  check(r.stats().sessions == 0, "slot returned");
}
void invalid_admission() {
  RuntimeManager r(limits()); std::atomic<int> calls{0};
  auto f = [&](const SessionHandle&) { ++calls; return SessionResources{}; };
  check(r.add("", f).status == AdmissionStatus::InvalidRequest, "empty ID");
  check(r.add(std::string(257, 'x'), f).status == AdmissionStatus::InvalidRequest, "long ID");
  check(r.add("a", {}).status == AdmissionStatus::InvalidRequest, "missing factory");
  check(calls == 0 && r.stats().rejected == 3, "invalid factories not called");
}
void concurrent_duplicate() {
  RuntimeManager r(limits(16)); std::atomic<int> calls{0};
  std::vector<SessionAdmission> results(12); std::vector<std::thread> threads;
  for (std::size_t i = 0; i < results.size(); ++i) threads.emplace_back([&, i] {
    results[i] = r.add("same", [&](const SessionHandle& h) { ++calls; return factory()(h); });
  });
  for (auto& t : threads) t.join();
  int accepted = 0;
  for (auto& a : results) if (a) { ++accepted; await(a.started); }
  check(accepted == 1 && calls == 1, "one atomic duplicate winner");
}
void concurrent_capacity() {
  RuntimeManager r(limits(3)); std::vector<SessionAdmission> results(12);
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < results.size(); ++i) threads.emplace_back([&, i] {
    results[i] = r.add(std::to_string(i), factory());
  });
  for (auto& t : threads) t.join();
  int accepted = 0; for (auto& a : results) if (a) ++accepted;
  check(accepted == 3 && r.stats().sessions == 3, "capacity atomic across callers");
}
void queue_full_and_remove_reserved() {
  auto c = limits(4); c.startup_workers = 1; c.max_pending_starts = 1;
  RuntimeManager r(c); Gate g; auto p = std::make_shared<Probe>();
  p->on_start = [&](ConversationEngineContext) { g.block(); };
  auto a = r.add("a", factory(p)); g.wait();
  std::atomic<int> calls{0};
  auto b = r.add("b", [&](const SessionHandle& h) { ++calls; return factory()(h); });
  check(bool(b), "one queued start");
  check(r.add("c", factory()).status == AdmissionStatus::StartupQueueFull, "queue rejection");
  r.remove(b.handle);
  check(await(b.started) == SessionOutcome::Removed, "queued startup cancelled");
  check(await(b.retired) == SessionOutcome::Removed && calls == 0, "skipped factory");
  check(r.stats().pending_starts == 0 && p->starts == 1,
        "reserved removal progresses while the startup worker is blocked");
  g.open(); await(a.started);
}
void factory_failure_rollback() {
  RuntimeManager r(limits(1));
  auto a = r.add("a", [](const SessionHandle&) -> SessionResources { throw std::runtime_error("fail"); });
  check(await(a.started) == SessionOutcome::StartupFailed, "factory fail startup ticket");
  check(await(a.retired) == SessionOutcome::StartupFailed, "factory fail retirement");
  auto b = r.add("a", factory()); check(bool(b), "failed reservation released"); await(b.started);
}
void null_engine_rollback() {
  RuntimeManager r(limits(1));
  auto a = r.add("a", [](const SessionHandle&) { return SessionResources{}; });
  check(await(a.retired) == SessionOutcome::StartupFailed, "invalid engine fails safely");
  check(r.stats().failures == 1 && r.stats().sessions == 0, "invalid engine releases slot");
}
void startup_failure_cleanup() {
  RuntimeManager r(limits(1)); auto p = std::make_shared<Probe>();
  p->on_start = [](ConversationEngineContext) { throw std::runtime_error("start"); };
  auto a = r.add("a", factory(p));
  check(await(a.started) == SessionOutcome::StartupFailed, "failed engine startup");
  await(a.retired); check(p->stops == 1 && p->destroyed == 1, "partial start cleaned once");
}
void remove_during_factory() {
  RuntimeManager r(limits(1)); Gate g; auto p = std::make_shared<Probe>();
  auto a = r.add("a", [&](const SessionHandle& h) { g.block(); return factory(p)(h); });
  g.wait(); r.remove(a.handle);
  check(r.add("b", factory()).status == AdmissionStatus::CapacityReached, "factory still charged");
  g.open(); check(await(a.started) == SessionOutcome::Removed, "factory result fenced");
  await(a.retired); check(p->starts == 0 && p->destroyed == 1, "unstarted engine cleaned");
}
void remove_during_start() {
  RuntimeManager r(limits(1)); Gate g; auto p = std::make_shared<Probe>();
  p->on_start = [&](ConversationEngineContext) { g.block(); };
  auto a = r.add("a", factory(p)); g.wait(); r.remove(a.handle);
  check(!r.push_audio(a.handle, {}), "starting removal closes input");
  check(a.retired.wait_for(5ms) == std::future_status::timeout, "start still owned");
  g.open(); check(await(a.started) == SessionOutcome::Removed, "no late running session");
  await(a.retired); check(p->stops == 1, "starting engine stopped");
}
void operation_lease_delays_retirement() {
  RuntimeManager r(limits(1)); Gate g; auto p = std::make_shared<Probe>();
  p->on_audio = [&] { g.block(); };
  auto a = r.add("a", factory(p)); await(a.started);
  auto input = std::async(std::launch::async, [&] { return r.push_audio(a.handle, {}); });
  g.wait(); r.remove(a.handle);
  check(r.find(a.handle)->admitted_operations == 1, "operation lease retained");
  check(r.add("b", factory()).status == AdmissionStatus::CapacityReached, "in-flight slot charged");
  check(p->destroyed == 0 && p->stops == 0, "no early teardown");
  g.open(); check(input.get(), "admitted operation completes"); await(a.retired);
}
void incarnations_and_runtime_scope() {
  RuntimeManager r(limits(1)), other(limits(1));
  auto a = r.add("a", factory()); await(a.started); r.remove(a.handle); await(a.retired);
  auto b = r.add("a", factory()); await(b.started);
  auto c = other.add("a", factory()); await(c.started);
  check(a.handle.generation != b.handle.generation, "incarnation changed");
  check(r.remove(a.handle) == RemoveStatus::StaleHandle, "late remove fenced");
  check(r.fail(a.handle) == RemoveStatus::StaleHandle && !r.push_audio(a.handle, {}), "late failure/input fenced");
  check(r.remove(c.handle) == RemoveStatus::StaleHandle, "wrong runtime fenced");
  check(r.push_audio(b.handle, {}), "replacement unaffected");
}
void engine_failure_isolation() {
  RuntimeManager r(limits()); auto p = std::make_shared<Probe>();
  p->on_audio = [] { throw std::runtime_error("input"); };
  auto a = r.add("a", factory(p)), b = r.add("b", factory());
  await(a.started); await(b.started);
  check(!r.push_audio(a.handle, {}), "exception contained at manager boundary");
  check(await(a.retired) == SessionOutcome::EngineFailure, "engine failure recorded");
  check(r.push_audio(b.handle, {}) && r.stats().failures == 1, "other session unaffected");
}
void fatal_report_and_error_observation() {
  RuntimeManager r(limits()); auto a = r.add("a", factory()); await(a.started);
  check(bool(r.observe(a.handle, {EventType::Error})), "recoverable observation accepted");
  check(r.push_audio(a.handle, {}), "Error observation does not imply fatal exit");
  r.fail(a.handle); check(await(a.retired) == SessionOutcome::ReportedFailure, "explicit fatal report");
}
void authoritative_failure_without_notifications() {
  RuntimeManager r(limits()); auto p = std::make_shared<Probe>();
  p->on_stop = [] { throw std::runtime_error("stop failed after quiescence"); };
  auto a = r.add("a", factory(p)); await(a.started); r.remove(a.handle);
  check(await(a.retired) == SessionOutcome::LifecycleFailure, "lifecycle failure reconciled without observers");
  check(r.stats().failures == 1, "terminal failure accounted");
}
void bounded_notifications_do_not_own_cleanup() {
  Gate observer; auto c = limits(); c.max_pending_notifications = 1;
  std::atomic<bool> first{true}; std::vector<std::uint64_t> sequences;
  RuntimeManager r(c, [&](const RuntimeNotification& n) {
    if (first.exchange(false)) observer.block();
    sequences.push_back(n.sequence);
  });
  auto a = r.add("a", factory()); observer.wait(); await(a.started);
  r.remove(a.handle); await(a.retired);
  auto b = r.add("b", factory()); await(b.started); r.fail(b.handle); await(b.retired);
  const auto stats = r.stats();
  check(stats.sessions == 0 && stats.retired == 2, "cleanup independent of notification sink");
  check(stats.dropped_notifications > 0 && stats.notification_high_watermark == 1,
        "bounded queue and explicit loss");
  check(stats.first_dropped_sequence && stats.last_dropped_sequence >= stats.first_dropped_sequence,
        "loss gap visible");
  observer.open(); check(r.flush_notifications(5s), "accepted notifications drained");
  for (std::size_t i = 1; i < sequences.size(); ++i)
    check(sequences[i] > sequences[i - 1], "ordered notification delivery");
}
void observer_failure_and_reentry() {
  RuntimeManager* runtime = nullptr; std::atomic<int> seen{0};
  RuntimeManager r(limits(), [&](const RuntimeNotification& n) {
    ++seen;
    (void)runtime->snapshot();
    check(!runtime->flush_notifications(1ms), "callback flush cannot self-wait");
    check(!runtime->wait_shutdown(1ms), "callback shutdown wait rejected");
    if (n.type == RuntimeNotificationType::Started) runtime->remove(n.handle);
    throw std::runtime_error("observer");
  });
  runtime = &r;
  auto a = r.add("a", factory()); await(a.started); await(a.retired);
  check(r.flush_notifications(5s), "throwing observers isolated");
  check(r.stats().observer_failures == static_cast<unsigned>(seen.load()), "observer failure counter");
}
void callback_shutdown() {
  RuntimeManager* runtime = nullptr;
  RuntimeManager r(limits(), [&](const RuntimeNotification& n) {
    if (n.type == RuntimeNotificationType::Started) runtime->shutdown();
  });
  runtime = &r; auto a = r.add("a", factory()); await(a.started);
  check(await(a.retired) == SessionOutcome::RuntimeShutdown, "callback requested shutdown");
  check(r.wait_shutdown(5s), "owner observes shutdown completion");
}
void session_observer_delays_slot_not_other_sessions() {
  Gate observer; auto c = limits(2); RuntimeManager r(c); auto p = std::make_shared<Probe>();
  p->on_start = [&](ConversationEngineContext ctx) {
    ctx.timeline->subscribe([&](const Event&) { observer.block(); });
  };
  auto a = r.add("a", factory(p)); await(a.started); observer.wait();
  auto b = r.add("b", factory()); await(b.started);
  r.remove(a.handle); r.remove(b.handle); await(b.retired);
  check(a.retired.wait_for(5ms) == std::future_status::timeout, "session callback still owned");
  check(p->destroyed == 0 && r.stats().sessions == 1, "callback keeps slot and engine");
  observer.open(); await(a.retired);
}
void dependency_order_and_reentry() {
  RuntimeManager r(limits(1)); auto p = std::make_shared<Probe>(); std::atomic<bool> destroyed{false};
  auto a = r.add("a", [&](const SessionHandle& h) {
    auto resources = factory(p)(h);
    resources.dependencies = std::shared_ptr<void>(new int(1), [&](void* ptr) {
      check(p->destroyed == 1, "engine destroyed before dependency");
      check(r.stats().sessions == 1 && r.stats().retiring == 1, "retiring slot still charged");
      destroyed = true; delete static_cast<int*>(ptr);
    });
    return resources;
  });
  await(a.started); r.remove(a.handle); await(a.retired);
  check(destroyed, "dependency destroyed before retirement resolves");
}
void shutdown_racing_start_and_idempotence() {
  RuntimeManager r(limits()); Gate g; auto p = std::make_shared<Probe>();
  p->on_start = [&](ConversationEngineContext) { g.block(); };
  auto a = r.add("a", factory(p)); g.wait(); r.request_shutdown();
  check(r.add("b", factory()).status == AdmissionStatus::Draining, "global admission closed");
  check(!r.wait_shutdown(5ms), "bounded wait retains stuck startup");
  g.open(); check(await(a.started) == SessionOutcome::RuntimeShutdown, "startup shutdown fenced");
  await(a.retired);
  std::thread t([&] { r.shutdown(); }); r.shutdown(); t.join();
  check(r.wait_shutdown(0ms) && r.stats().sessions == 0 && p->stops == 1, "shared shutdown joined once");
}
void factory_capture_destroyed_without_lock() {
  RuntimeManager r(limits()); std::atomic<bool> released{false};
  auto token = std::shared_ptr<void>(new int(0), [&](void* ptr) {
    (void)r.stats(); released = true; delete static_cast<int*>(ptr);
  });
  auto f = [token](const SessionHandle& h) { return factory()(h); };
  token.reset();
  auto a = r.add("a", std::move(f)); await(a.started);
  check(released, "factory capture destructor may re-enter runtime");
}
void idempotent_remove_and_failure_escalation() {
  Gate stop; RuntimeManager r(limits(1)); auto p = std::make_shared<Probe>();
  p->on_stop = [&] { stop.block(); };
  auto a = r.add("a", factory(p)); await(a.started);
  check(r.remove(a.handle) == RemoveStatus::Requested, "first remove requested"); stop.wait();
  check(r.remove(a.handle) == RemoveStatus::AlreadyRequested, "duplicate remove idempotent");
  check(r.fail(a.handle) == RemoveStatus::AlreadyRequested, "fatal report escalates existing stop");
  check(r.find(a.handle)->outcome == SessionOutcome::ReportedFailure, "fatal cause retained");
  stop.open(); check(await(a.retired) == SessionOutcome::ReportedFailure && p->stops == 1,
                     "one terminal cleanup");
}
void engine_callback_shutdown() {
  RuntimeManager r(limits()); auto p = std::make_shared<Probe>();
  p->on_audio = [&] { r.shutdown(); };
  auto a = r.add("a", factory(p)); await(a.started);
  check(r.push_audio(a.handle, {}), "engine callback returned without joining itself");
  check(await(a.retired) == SessionOutcome::RuntimeShutdown, "engine callback shutdown");
  check(r.wait_shutdown(5s), "owner can wait afterwards");
}
void observer_shutdown_drain() {
  Gate g; RuntimeManager r(limits(), [&](const RuntimeNotification&) { g.block(); });
  auto a = r.add("a", factory()); g.wait(); await(a.started);
  r.request_shutdown(); await(a.retired);
  check(!r.wait_shutdown(5ms) && r.stats().sessions == 0,
        "shutdown still owns in-flight runtime observer after sessions retire");
  g.open(); check(r.wait_shutdown(5s), "observer quiescence precedes stopped");
}
void event_identity_validation() {
  RuntimeManager r(limits()); auto a = r.add("a", factory()); await(a.started);
  Event wrong(EventType::InputSpeechStarted, "other");
  check(r.observe(a.handle, wrong).status == TimelineStatus::WrongSession, "foreign event rejected");
  Event stale(EventType::InputSpeechStarted); stale.generation = a.handle.generation + 1;
  check(r.observe(a.handle, stale).status == TimelineStatus::StaleGeneration, "stale event rejected");
  check(bool(r.observe(a.handle, {EventType::InputSpeechStarted})), "matching identity normalized");
  check(!r.handle_event(a.handle, wrong), "foreign compatibility control rejected");
}
void dependency_retirement_holds_capacity() {
  Gate teardown; RuntimeManager r(limits(2)); auto p = std::make_shared<Probe>();
  auto a = r.add("a", [&](const SessionHandle& h) {
    auto resources = factory(p)(h);
    resources.dependencies = std::shared_ptr<void>(new int(1), [&](void* ptr) {
      teardown.block(); delete static_cast<int*>(ptr);
    });
    return resources;
  });
  std::promise<void> stopped; auto stopped_future = stopped.get_future();
  auto other = std::make_shared<Probe>(); other->on_stop = [&] { stopped.set_value(); };
  auto b = r.add("b", factory(other)); await(a.started); await(b.started);
  r.remove(a.handle); teardown.wait();
  check(r.add("c", factory()).status == AdmissionStatus::CapacityReached, "teardown remains charged");
  r.remove(b.handle);
  check(stopped_future.wait_for(5s) == std::future_status::ready,
        "slow teardown cannot block stop delivery to another session");
  check(a.retired.wait_for(5ms) == std::future_status::timeout, "no early retirement ticket");
  teardown.open(); await(a.retired); await(b.retired);
}
void repeated_add_remove_stress() {
  RuntimeManager r(limits(8));
  for (int round = 0; round < 20; ++round) {
    std::vector<SessionAdmission> batch;
    for (int i = 0; i < 6; ++i) batch.push_back(r.add(std::to_string(i), factory()));
    for (auto& a : batch) { check(bool(a), "stress admission"); await(a.started); }
    std::vector<std::thread> removers;
    for (auto& a : batch) removers.emplace_back([&, h = a.handle] { r.remove(h); });
    for (auto& t : removers) t.join();
    for (auto& a : batch) await(a.retired);
    check(r.stats().sessions == 0, "stress slots returned");
  }
  check(r.stats().retired == 120, "stress no duplicate/lost retirement");
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
    {"admission and retirement", admission_and_retirement},
    {"invalid admission", invalid_admission},
    {"concurrent duplicate", concurrent_duplicate},
    {"concurrent capacity", concurrent_capacity},
    {"queue full and reserved removal", queue_full_and_remove_reserved},
    {"factory failure rollback", factory_failure_rollback},
    {"null engine rollback", null_engine_rollback},
    {"startup failure cleanup", startup_failure_cleanup},
    {"remove during factory", remove_during_factory},
    {"remove during start", remove_during_start},
    {"operation lease", operation_lease_delays_retirement},
    {"incarnations and runtime scope", incarnations_and_runtime_scope},
    {"engine failure isolation", engine_failure_isolation},
    {"fatal report versus observation", fatal_report_and_error_observation},
    {"authoritative failure without notifications", authoritative_failure_without_notifications},
    {"bounded notifications", bounded_notifications_do_not_own_cleanup},
    {"observer failures and reentry", observer_failure_and_reentry},
    {"callback shutdown", callback_shutdown},
    {"session callback drain", session_observer_delays_slot_not_other_sessions},
    {"dependency order and reentry", dependency_order_and_reentry},
    {"factory capture reentry", factory_capture_destroyed_without_lock},
    {"shutdown race and idempotence", shutdown_racing_start_and_idempotence},
    {"idempotent removal and failure escalation", idempotent_remove_and_failure_escalation},
    {"engine callback shutdown", engine_callback_shutdown},
    {"runtime observer shutdown drain", observer_shutdown_drain},
    {"event identity validation", event_identity_validation},
    {"dependency retirement capacity", dependency_retirement_holds_capacity},
    {"repeated concurrent retirement", repeated_add_remove_stress}
  };
  for (auto& test : tests) {
    try { test.second(); std::cout << "PASS " << test.first << std::endl; }
    catch (const std::exception& e) {
      std::cerr << "FAIL " << test.first << ": " << e.what() << std::endl; return 1;
    }
  }
  std::cout << tests.size() << " runtime manager tests passed\n";
}
