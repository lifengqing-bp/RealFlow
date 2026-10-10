// Linux/libstdc++ linker interposition forces the check-to-wait window without
// production hooks. Only this executable wraps these symbols.
#include "test_support.h"
#include "byteturn/full_duplex_conversation.h"

#include <atomic>
#include <cstdlib>
#include <pthread.h>
#include <thread>

namespace {
using namespace byteturn;
using namespace realflow_test;
// The worker holds the input mutex after checking a false wait predicate.
// A correct closer attempts that same mutex before changing the predicate.
enum class Phase { Armed, Parked, LockAttempted, EarlyNotification };
std::atomic<Phase> phase{Phase::Armed};
pthread_mutex_t* input_mutex = nullptr;
std::condition_variable* input_cv = nullptr;

void wait_until(const std::function<bool()>& ready, const char* diagnostic) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << diagnostic << std::endl;
      std::_Exit(2);  // A failed schedule must not hang in owner destruction.
    }
    std::this_thread::yield();
  }
}

class Session final : public RealtimeSpeechSession {
 public:
  explicit Session(int& closes) : closes_(closes) {}
  bool push_audio(const AudioFrame&) override { return true; }
  void commit_input() override {}
  void cancel_response(const std::string&, std::uint64_t, int) override {}
  void close() override { ++closes_; }
 private:
  int& closes_;
};
class Provider final : public RealtimeSpeechProvider {
 public:
  int closes = 0;
  std::unique_ptr<RealtimeSpeechSession> connect(const RealtimeSessionConfig&, EventSink) override {
    return std::make_unique<Session>(closes);
  }
};
}

// These are libstdc++'s out-of-line wait/notify entry points. Wrapping just
// pthread_cond_wait would miss calls originating inside the shared library.
extern "C" void real_wait(std::condition_variable*, std::unique_lock<std::mutex>&)
    asm("__real__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE");
extern "C" void wrapped_wait(std::condition_variable* cv, std::unique_lock<std::mutex>& lock)
    asm("__wrap__ZNSt18condition_variable4waitERSt11unique_lockISt5mutexE");
extern "C" void wrapped_wait(std::condition_variable* cv, std::unique_lock<std::mutex>& lock) {
  if (phase.load() == Phase::Armed) {
    input_mutex = lock.mutex()->native_handle();
    input_cv = cv;
    phase.store(Phase::Parked);
    wait_until([] { return phase.load() != Phase::Parked; }, "worker never released at check-to-wait gate");
    // With the old close(), notify happened before the actual wait. Return a
    // permitted spurious wake instead of hanging, then fail on the test thread.
    if (phase.load() == Phase::EarlyNotification) return;
  }
  real_wait(cv, lock);
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (phase.load() == Phase::Parked && mutex == input_mutex)
    phase.store(Phase::LockAttempted);
  return __real_pthread_mutex_lock(mutex);
}
extern "C" void real_notify(std::condition_variable*)
    asm("__real__ZNSt18condition_variable10notify_allEv");
extern "C" void wrapped_notify(std::condition_variable* cv)
    asm("__wrap__ZNSt18condition_variable10notify_allEv");
extern "C" void wrapped_notify(std::condition_variable* cv) {
  real_notify(cv);
  if (phase.load() == Phase::Parked && cv == input_cv)
    phase.store(Phase::EarlyNotification);
}

int main(int argc, char** argv) {
  return run(argc, argv, {{"native_close_serializes_with_wait_predicate", [] {
    Provider p;
    EventBus bus;
    int closed_events = 0;
    bus.subscribe([&](const Event& e) {
      if (e.type == EventType::RealtimeSessionClosed) ++closed_events;
    });
    {
      FullDuplexConversation s(p, "close-race", {}, [](const DuplexAudio&) { return true; }, &bus);
      wait_until([] { return phase.load() == Phase::Parked; }, "input worker never reached check-to-wait gate");
      s.close();
      check(phase.load() == Phase::LockAttempted,
            "close notified between predicate check and wait without acquiring the input mutex");
      s.close();
      check(!s.push_audio({{1}, 16000, 1, false}), "closed input rejects audio");
    }
    check(p.closes == 1 && closed_events == 1, "provider close and terminal event exactly once");
  }}});
}
