#include "byteturn/conversation_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace byteturn;
using namespace std::chrono_literals;

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class Gate {
 public:
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, 5s, [&] { return open_; }))
      throw std::runtime_error("test gate timed out");
  }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

template<class F> void throws(F&& call) {
  bool caught = false;
  try { call(); } catch (const std::exception&) { caught = true; }
  check(caught, "expected an exception");
}

template<class T> void ready(std::future<T>& result) {
  check(result.wait_for(5s) == std::future_status::ready, "future timed out");
}

class ScriptedEngine final : public ConversationEngine {
 public:
  void start(ConversationEngineContext value) override {
    ++starts;
    context = std::move(value);
    if (on_start) on_start();
  }
  bool push_audio(AudioFrame) override {
    ++pushes;
    if (on_push) on_push();
    return true;
  }
  void handle_event(const Event& event) override {
    if (on_event) on_event(event);
    std::lock_guard<std::mutex> lock(mutex_);
    forwarded.push_back(event);
    if (event.type == EventType::InputSpeechStarted) state_.user_speaking = true;
    if (event.type == EventType::SpeechStarted) state_.agent_speaking = true;
  }
  void stop() override {
    ++stops;
    if (on_stop) on_stop();
  }
  ConversationCapabilities capabilities() const override {
    ConversationCapabilities caps;
    caps.simultaneous_listen_speak = true;
    return caps;
  }
  ConversationStateSnapshot state() const override {
    if (on_state) on_state();
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }
  ConversationEngineContext context;
  std::function<void()> on_start, on_push, on_stop, on_state;
  std::function<void(const Event&)> on_event;
  std::atomic<int> starts{0}, stops{0}, pushes{0};
  std::vector<Event> forwarded;
 private:
  mutable std::mutex mutex_;
  ConversationStateSnapshot state_;
};

void same_event(const Event& a, const Event& b) {
  check(a.type == b.type && a.session_id == b.session_id &&
        a.turn_id == b.turn_id && a.sequence == b.sequence &&
        a.timestamp == b.timestamp && a.received_at == b.received_at &&
        a.generation == b.generation && a.trace_id == b.trace_id &&
        a.name == b.name && a.data == b.data, "stored/delivered envelope differs");
}

void canonical_metadata() {
  TimelineConfig config;
  config.session_id = "voice";
  config.generation = 7;
  auto now = std::chrono::steady_clock::time_point(100ms);
  config.clock = [&] { now += 1ms; return now; };
  EventTimeline timeline(nullptr, config);
  std::vector<Event> observed;
  timeline.subscribe([&](const Event& e) { observed.push_back(e); });
  Event canonical(EventType::Error);
  auto first = timeline.append({EventType::InputSpeechStarted, {}, "u1", 99},
                               &canonical);
  const auto source_time = std::chrono::steady_clock::time_point(17ms);
  auto second = timeline.append(
      {EventType::InputSpeechEnded, "voice", "u1", 200, source_time});
  check(first && second && timeline.flush(), "admission/flush failed");
  const auto stored = timeline.snapshot();
  check(stored.size() == 2 && observed.size() == 2, "missing event");
  same_event(stored[0], observed[0]);
  same_event(stored[1], observed[1]);
  same_event(stored[0], canonical);
  check(stored[0].sequence == 1 && stored[1].sequence == 2,
        "timeline must own sequence assignment");
  check(stored[0].session_id == "voice" && stored[0].trace_id == "voice:u1" &&
        stored[0].generation == 7, "scope metadata missing");
  check(stored[0].timestamp == stored[0].received_at &&
        stored[1].timestamp == source_time &&
        stored[1].received_at == std::chrono::steady_clock::time_point(102ms),
        "source and receive clocks were conflated");
}

void concurrent_admission_order() {
  TimelineConfig config;
  config.max_events = config.max_pending_notifications = 2048;
  EventTimeline timeline(nullptr, config);
  std::vector<Event> observed;
  timeline.subscribe([&](const Event& e) { observed.push_back(e); });
  std::vector<std::thread> producers;
  std::atomic<int> rejected{0};
  for (int p = 0; p < 8; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < 200; ++i)
        if (!timeline.append({EventType::ModelTextDelta, "s", {}, 999, {},
                              std::to_string(p), std::to_string(i)})) ++rejected;
    });
  }
  for (auto& p : producers) p.join();
  check(timeline.flush(), "concurrent delivery did not drain");
  const auto stored = timeline.snapshot();
  check(rejected == 0 && stored.size() == 1600 && observed.size() == stored.size(),
        "concurrent events lost");
  for (std::size_t i = 0; i < stored.size(); ++i) {
    check(stored[i].sequence == i + 1, "sequence not contiguous");
    same_event(stored[i], observed[i]);
  }
}

void retention_validation_and_late_sink() {
  TimelineConfig config;
  config.session_id = "s";
  config.generation = 4;
  config.max_events = 3;
  config.max_event_bytes = 32;
  EventTimeline::Sink late;
  {
    EventTimeline timeline(nullptr, config);
    late = timeline.sink();
    for (int i = 0; i < 10; ++i)
      check(bool(late({EventType::InputSpeechStarted})), "valid event rejected");
    const auto stats = timeline.stats();
    check(stats.accepted == 10 && stats.evicted == 7 && stats.retained == 3 &&
          stats.oldest_sequence == 8 && stats.newest_sequence == 10,
          "retention bounds/gaps incorrect");
    check(timeline.append({EventType::Error, "other"}).status ==
              TimelineStatus::WrongSession, "foreign session accepted");
    Event stale(EventType::Error);
    stale.generation = 3;
    check(timeline.append(stale).status == TimelineStatus::StaleGeneration,
          "stale generation accepted");
    check(timeline.append({EventType::Error, {}, {}, 0, {}, {}, std::string(100, 'x')})
              .status == TimelineStatus::EventTooLarge, "oversized event accepted");
    check(timeline.stats().rejected == 3 && timeline.size() == 3,
          "rejections changed the journal");
    timeline.close();
    check(late({EventType::Error}).status == TimelineStatus::Closed,
          "closed timeline accepted a late event");
  }
  check(late({EventType::Error}).status == TimelineStatus::Closed,
        "weak sink outlived its journal unsafely");
}

void invalid_timeline_config() {
  TimelineConfig config;
  config.max_events = 0;
  throws([&] { EventTimeline timeline(nullptr, config); });
  config.max_events = 1;
  config.max_pending_notifications = 0;
  throws([&] { EventTimeline timeline(nullptr, config); });
  config.max_pending_notifications = 1;
  config.max_event_bytes = 0;
  throws([&] { EventTimeline timeline(nullptr, config); });
  config.max_event_bytes = 1;
  config.generation = 0;
  throws([&] { EventTimeline timeline(nullptr, config); });
}

void bounded_slow_observer() {
  TimelineConfig config;
  config.max_events = 8;
  config.max_pending_notifications = 2;
  EventTimeline timeline(nullptr, config);
  Gate entered, release;
  std::vector<std::uint64_t> observed;
  timeline.subscribe([&](const Event& e) {
    if (e.sequence == 1) { entered.open(); release.wait(); }
    observed.push_back(e.sequence);
  });
  timeline.append({EventType::InputSpeechStarted});
  entered.wait();
  for (int i = 2; i <= 10; ++i) {
    const auto result = timeline.append({EventType::AudioOutput});
    check(bool(result), "slow observer must not reject control admission");
    check(result.notification_enqueued == (i <= 3), "queue did not honor capacity");
  }
  auto stats = timeline.stats();
  check(stats.retained == 8 && stats.evicted == 2 && stats.pending_notifications == 2 &&
        stats.notification_high_watermark == 2 && stats.dropped_notifications == 7 &&
        stats.first_dropped_sequence == 4 && stats.last_dropped_sequence == 10,
        "observer overload accounting incorrect");
  check(!timeline.flush(1ms), "flush should time out on blocked observer");
  release.open();
  check(timeline.flush(), "observer did not recover");
  timeline.append({EventType::InputSpeechEnded});
  check(timeline.flush(), "post-overload notification did not drain");
  check(observed == std::vector<std::uint64_t>({1, 2, 3, 11}),
        "observer gap was hidden or ordering changed");
}

void observer_failure_and_reentry() {
  EventTimeline timeline;
  std::vector<std::uint64_t> good;
  timeline.subscribe([](const Event&) { throw std::runtime_error("observer failed"); });
  timeline.subscribe([&](const Event& e) { good.push_back(e.sequence); });
  EventTimeline::Subscription self = 0;
  std::atomic<int> self_calls{0};
  self = timeline.subscribe([&](const Event&) {
    ++self_calls;
    timeline.append({EventType::InputSpeechEnded});
    check(!timeline.flush(1ms), "callback flush must not self-wait");
    check(timeline.snapshot().size() == 2, "reentrant snapshot is not safe");
    timeline.unsubscribe(self);
  });
  timeline.append({EventType::InputSpeechStarted});
  // The second event may be enqueued during the first flush: drain twice.
  check(timeline.flush() && timeline.flush(), "reentrant delivery did not drain");
  check(good == std::vector<std::uint64_t>({1, 2}) && self_calls == 1,
        "recursive admission/unsubscribe failed");
  check(timeline.stats().observer_failures == 2, "observer failures not counted");
}

void nested_ancestor_unsubscription() {
  EventBus outer, inner;
  int calls = 0;
  EventBus::Subscription ancestor = 0;
  inner.subscribe([&](const Event&) { outer.unsubscribe(ancestor); });
  ancestor = outer.subscribe([&](const Event&) {
    ++calls;
    inner.publish({EventType::Error});
  });
  outer.publish({EventType::Error});
  outer.publish({EventType::Error});
  check(calls == 1, "ancestor unsubscribe failed");
  throws([&] { outer.subscribe({}); });
}

void external_unsubscription_quiesces() {
  EventTimeline timeline;
  Gate entered, release;
  auto id = timeline.subscribe([&](const Event&) { entered.open(); release.wait(); });
  timeline.append({EventType::Error});
  entered.wait();
  auto removal = std::async(std::launch::async, [&] { timeline.unsubscribe(id); });
  check(removal.wait_for(10ms) == std::future_status::timeout,
        "unsubscribe returned before in-flight observer ended");
  release.open();
  ready(removal); removal.get();
  check(timeline.flush(), "unsubscription blocked delivery");
}

void session_lifecycle_and_overlap() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  ConversationSession session("s", std::move(engine));
  check(!session.push_audio({}) && !session.observe({EventType::Error}),
        "pre-start input accepted");
  session.start(); session.start();
  check(raw->starts == 1 && session.lifecycle() == SessionLifecycle::Running,
        "session started more than once");
  check(session.capabilities().simultaneous_listen_speak, "capabilities missing");
  session.handle_event({EventType::SpeechStarted});
  session.handle_event({EventType::InputSpeechStarted});
  check(session.state().user_speaking && session.state().agent_speaking,
        "overlap invariant broken");
  const auto forwarded = raw->forwarded;
  check(forwarded.size() == 2 && forwarded[0].sequence == 2 &&
        forwarded[1].sequence == 3 && forwarded[0].session_id == "s",
        "engine did not receive canonical metadata");
  session.stop(); session.stop();
  check(raw->stops == 1 && session.lifecycle() == SessionLifecycle::Stopped,
        "stop must quiesce once");
  check(!session.push_audio({}) && !session.observe({EventType::Error}),
        "post-stop input accepted");
  check(raw->context.emit({EventType::Error}).status == TimelineStatus::Closed,
        "provider result not fenced after stop");
  throws([&] { session.start(); });
  const auto events = session.timeline().snapshot();
  check(events.size() == 4 && events.front().type == EventType::SessionStarted &&
        events.back().type == EventType::SessionStopped, "lifecycle events missing");
}

void session_validation_and_observation_only() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  TimelineConfig config;
  config.generation = 9;
  ConversationSession session("s", std::move(engine), nullptr, config);
  session.start();
  check(raw->context.generation == 9, "engine generation did not match ingress");
  session.handle_event({EventType::InputSpeechStarted, "foreign"});
  Event stale(EventType::InputSpeechStarted);
  stale.generation = 8;
  session.handle_event(stale);
  check(raw->forwarded.empty(), "invalid input reached the engine");
  check(bool(session.observe({EventType::InputSpeechStarted})), "observation rejected");
  check(raw->forwarded.empty(), "observation executed an engine action");
  check(session.timeline().stats().rejected == 2, "invalid ingress not accounted");
}

void stop_before_start() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  ConversationSession session("s", std::move(engine));
  session.stop();
  check(raw->starts == 0 && raw->stops == 0 &&
        session.lifecycle() == SessionLifecycle::Stopped, "created shutdown failed");
  throws([&] { session.start(); });
}

void failed_start_cleans_resources() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  bool allocated = false;
  raw->on_start = [&] { allocated = true; throw std::runtime_error("startup failed"); };
  raw->on_stop = [&] { allocated = false; throw std::runtime_error("cleanup reported failure"); };
  ConversationSession session("s", std::move(engine));
  std::string error;
  try { session.start(); } catch (const std::runtime_error& e) { error = e.what(); }
  check(error == "startup failed" && !allocated && raw->stops == 1 &&
        session.lifecycle() == SessionLifecycle::Failed && bool(session.failure()),
        "startup failure was not cleaned up or original error was lost");
  session.stop();
  check(raw->stops == 1, "failed startup cleaned up twice");
  throws([&] { session.start(); });
  check(session.timeline().snapshot().back().type == EventType::SessionFailed,
        "startup failure event missing");
}

void failed_stop_is_reported_not_thrown() {
  auto engine = std::make_unique<ScriptedEngine>();
  engine->on_stop = [] { throw std::runtime_error("shutdown failed"); };
  ConversationSession session("s", std::move(engine));
  session.start();
  session.stop();
  check(session.lifecycle() == SessionLifecycle::Failed && bool(session.failure()),
        "shutdown exception not captured");
}

void concurrent_start_is_single() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  Gate entered, release;
  raw->on_start = [&] { entered.open(); release.wait(); };
  ConversationSession session("s", std::move(engine));
  auto a = std::async(std::launch::async, [&] { session.start(); });
  entered.wait();
  auto b = std::async(std::launch::async, [&] { session.start(); });
  release.open();
  ready(a); ready(b); a.get(); b.get();
  check(raw->starts == 1, "concurrent start ran twice");
}

void stop_during_start() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  Gate entered, release;
  raw->on_start = [&] { entered.open(); release.wait(); };
  ConversationSession session("s", std::move(engine));
  auto start = std::async(std::launch::async, [&] { throws([&] { session.start(); }); });
  entered.wait();
  session.request_stop();
  check(!session.push_audio({}), "stopping startup admitted input");
  release.open();
  ready(start); start.get();
  check(session.wait_stopped(5s) && raw->stops == 1, "startup/stop race leaked cleanup");
}

void shutdown_waits_for_admitted_input() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  Gate entered, release;
  std::atomic<bool> active{false}, raced{false};
  raw->on_push = [&] { active = true; entered.open(); release.wait(); active = false; };
  raw->on_stop = [&] { if (active) raced = true; };
  ConversationSession session("s", std::move(engine));
  session.start();
  auto push = std::async(std::launch::async, [&] { return session.push_audio({}); });
  entered.wait();
  session.request_stop();
  check(!session.push_audio({}) && !session.wait_stopped(1ms) && raw->stops == 0,
        "stop failed to close admission or drained too early");
  auto a = std::async(std::launch::async, [&] { session.stop(); });
  auto b = std::async(std::launch::async, [&] { session.stop(); });
  release.open();
  ready(push); ready(a); ready(b);
  check(push.get(), "accepted push was lost"); a.get(); b.get();
  check(!raced && raw->stops == 1, "engine teardown raced accepted input");
}

void shutdown_waits_for_state_read() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  Gate entered, release;
  raw->on_state = [&] { entered.open(); release.wait(); };
  ConversationSession session("s", std::move(engine));
  session.start();
  auto state = std::async(std::launch::async, [&] { return session.state(); });
  entered.wait();
  session.request_stop();
  check(raw->stops == 0 && !session.wait_stopped(1ms), "state read not leased");
  release.open();
  ready(state); (void)state.get();
  check(session.wait_stopped(5s) && raw->stops == 1, "state lease did not drain");
}

void engine_exception_releases_admission() {
  auto engine = std::make_unique<ScriptedEngine>();
  engine->on_push = [] { throw std::runtime_error("push failed"); };
  ConversationSession session("s", std::move(engine));
  session.start();
  throws([&] { session.push_audio({}); });
  session.stop();
  check(session.lifecycle() == SessionLifecycle::Stopped, "exception leaked a lease");
}

void reentrant_stop_from_engine_input() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  ConversationSession session("s", std::move(engine));
  raw->on_push = [&] {
    session.stop();
    check(!session.wait_stopped(1ms), "engine callback must not wait on its lease");
  };
  session.start();
  check(session.push_audio({}), "reentrant stop corrupted admitted call");
  check(session.wait_stopped(5s) && raw->stops == 1, "reentrant stop was not deferred");
}

void reentrant_stop_from_start_and_stop() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  ConversationSession session("s", std::move(engine));
  raw->on_start = [&] { session.stop(); };
  raw->on_stop = [&] { session.stop(); };
  throws([&] { session.start(); });
  check(session.wait_stopped(5s) && raw->stops == 1, "lifecycle callback self-waited");
}

void reentrant_observer_stop() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  ConversationSession session("s", std::move(engine));
  Gate returned;
  session.timeline().subscribe([&](const Event& e) {
    if (e.type == EventType::InputSpeechStarted) {
      session.stop();
      returned.open();
    }
  });
  session.start();
  session.observe({EventType::InputSpeechStarted});
  returned.wait();
  check(session.wait_stopped(5s) && raw->stops == 1, "observer stop did not complete");
  check(session.timeline().flush(), "observer completion not drained");
}

void callback_start_does_not_deadlock() {
  auto engine = std::make_unique<ScriptedEngine>();
  auto* raw = engine.get();
  Gate start_entered, release_start, callback_done;
  raw->on_start = [&] {
    raw->context.emit({EventType::InputSpeechStarted});
    start_entered.open();
    release_start.wait();
  };
  ConversationSession session("s", std::move(engine));
  std::atomic<bool> rejected{false};
  session.timeline().subscribe([&](const Event& e) {
    if (e.type == EventType::InputSpeechStarted) {
      try { session.start(); } catch (const std::logic_error&) { rejected = true; }
      callback_done.open();
    }
  });
  auto startup = std::async(std::launch::async, [&] { session.start(); });
  start_entered.wait(); callback_done.wait();
  release_start.open();
  ready(startup); startup.get();
  check(rejected, "callback waited for startup instead of rejecting re-entry");
}

void destructor_drains_observers() {
  Gate entered, release;
  auto engine = std::make_unique<ScriptedEngine>();
  auto session = std::make_unique<ConversationSession>("s", std::move(engine));
  session->timeline().subscribe([&](const Event& e) {
    if (e.type == EventType::InputSpeechStarted) { entered.open(); release.wait(); }
  });
  session->start();
  session->observe({EventType::InputSpeechStarted});
  entered.wait();
  auto destroy = std::async(std::launch::async, [s = std::move(session)]() mutable {
    s.reset();
  });
  check(destroy.wait_for(10ms) == std::future_status::timeout,
        "destructor did not quiesce observer delivery");
  release.open();
  ready(destroy); destroy.get();
}

void concurrent_input_shutdown_stress() {
  for (int round = 0; round < 40; ++round) {
    auto engine = std::make_unique<ScriptedEngine>();
    auto* raw = engine.get();
    ConversationSession session("s", std::move(engine));
    session.start();
    std::vector<std::thread> threads;
    for (int p = 0; p < 4; ++p) threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        session.push_audio({});
        session.handle_event({EventType::InputSpeechStarted});
        (void)session.state();
        (void)session.capabilities();
      }
    });
    threads.emplace_back([&] { session.stop(); });
    threads.emplace_back([&] { session.stop(); });
    for (auto& t : threads) t.join();
    check(raw->stops == 1 && session.lifecycle() == SessionLifecycle::Stopped,
          "stress shutdown failed");
  }
}
}  // namespace

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
      {"canonical metadata", canonical_metadata},
      {"concurrent admission order", concurrent_admission_order},
      {"bounded retention / validation / late sink", retention_validation_and_late_sink},
      {"invalid timeline configuration", invalid_timeline_config},
      {"bounded slow observer", bounded_slow_observer},
      {"observer failure and re-entry", observer_failure_and_reentry},
      {"nested ancestor unsubscription", nested_ancestor_unsubscription},
      {"external unsubscription quiesces", external_unsubscription_quiesces},
      {"session lifecycle and overlap", session_lifecycle_and_overlap},
      {"observation-only ingress and validation", session_validation_and_observation_only},
      {"stop before start", stop_before_start},
      {"failed start cleanup", failed_start_cleans_resources},
      {"failed stop reporting", failed_stop_is_reported_not_thrown},
      {"concurrent start", concurrent_start_is_single},
      {"stop during start", stop_during_start},
      {"shutdown drains admitted input", shutdown_waits_for_admitted_input},
      {"shutdown drains state reads", shutdown_waits_for_state_read},
      {"engine exception releases admission", engine_exception_releases_admission},
      {"reentrant stop from engine input", reentrant_stop_from_engine_input},
      {"reentrant lifecycle callbacks", reentrant_stop_from_start_and_stop},
      {"reentrant observer stop", reentrant_observer_stop},
      {"callback start rejects self-wait", callback_start_does_not_deadlock},
      {"destructor drains observers", destructor_drains_observers},
      {"concurrent input/shutdown stress", concurrent_input_shutdown_stress},
  };
  for (const auto& test : tests) {
    try { test.second(); }
    catch (const std::exception& e) {
      std::cerr << "FAIL " << test.first << ": " << e.what() << '\n';
      return 1;
    }
    std::cout << "PASS " << test.first << '\n';
  }
  std::cout << tests.size() << " runtime foundation tests passed\n";
}
