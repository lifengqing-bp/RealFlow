#include "byteturn/runtime_manager.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace byteturn {
namespace {
struct RuntimeCall {
  explicit RuntimeCall(const void* value);
  ~RuntimeCall();
  const void* owner;
  RuntimeCall* previous;
};
thread_local RuntimeCall* current_call = nullptr;
RuntimeCall::RuntimeCall(const void* value) : owner(value), previous(current_call) {
  current_call = this;
}
RuntimeCall::~RuntimeCall() { current_call = previous; }

std::uint64_t next_runtime_id() {
  static std::atomic<std::uint64_t> next{1};
  auto id = next.load();
  do {
    if (id == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("runtime identity exhausted");
  } while (!next.compare_exchange_weak(id, id + 1));
  return id;
}
bool failed(SessionOutcome outcome) {
  return outcome == SessionOutcome::StartupFailed ||
         outcome == SessionOutcome::EngineFailure ||
         outcome == SessionOutcome::ReportedFailure ||
         outcome == SessionOutcome::LifecycleFailure ||
         outcome == SessionOutcome::UnexpectedStop;
}
bool terminal(SessionLifecycle lifecycle) {
  return lifecycle == SessionLifecycle::Stopped || lifecycle == SessionLifecycle::Failed;
}
}  // namespace

struct RuntimeManager::Impl {
  struct Entry {
    SessionHandle handle;
    SessionFactory factory;
    std::shared_ptr<void> dependencies;
    std::unique_ptr<ConversationSession> session;
    SessionPhase phase = SessionPhase::Reserved;
    SessionOutcome outcome = SessionOutcome::None;
    std::size_t operations = 0;
    bool stop_requested = false;
    bool startup_done = false;
    bool cancelled_before_start = false;
    std::promise<SessionOutcome> started, retired;
  };

  RuntimeConfig config;
  NotifySink notify;
  const std::uint64_t runtime_id = next_runtime_id();
  mutable std::mutex mutex;
  std::condition_variable cv;
  std::unordered_map<std::string, std::shared_ptr<Entry>> registry;
  std::deque<std::shared_ptr<Entry>> pending;
  std::deque<RuntimeNotification> notifications;
  std::vector<std::shared_ptr<Entry>> scan_entries;
  RuntimeStats counters;
  std::uint64_t generation = 1, sequence = 1, enqueued = 0, delivered = 0;
  std::uint64_t revision = 0;
  std::size_t workers_done = 0;
  bool abort_construction = false, supervisor_done = false;
  std::vector<std::thread> workers;
  std::thread supervisor, dispatcher;
  std::mutex join_mutex;

  explicit Impl(RuntimeConfig value, NotifySink observer)
      : config(value), notify(std::move(observer)) {
    if (!config.max_sessions || !config.max_pending_starts ||
        !config.startup_workers || !config.max_pending_notifications ||
        !config.max_session_id_bytes || config.supervisor_interval.count() <= 0)
      throw std::invalid_argument("runtime limits must be positive");
    registry.reserve(config.max_sessions);
    workers.reserve(config.startup_workers);
    scan_entries.reserve(config.max_sessions);
    try {
      for (std::size_t i = 0; i < config.startup_workers; ++i)
        workers.emplace_back([this] { startup_loop(); });
      supervisor = std::thread([this] { supervise(); });
      dispatcher = std::thread([this] { dispatch(); });
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        abort_construction = true;
      }
      cv.notify_all();
      join();
      throw;
    }
  }

  bool callback() const noexcept {
    if (EventBus::in_callback()) return true;
    for (auto* call = current_call; call; call = call->previous)
      if (call->owner == this) return true;
    return false;
  }

  void changed() { ++revision; cv.notify_all(); }

  // Observations are expendable; membership and promises are not.
  void publish(const Entry& entry, RuntimeNotificationType type) noexcept {
    if (!notify) return;
    const auto number = sequence;
    if (sequence != std::numeric_limits<std::uint64_t>::max()) ++sequence;
    else {
      ++counters.dropped_notifications;
      if (!counters.first_dropped_sequence) counters.first_dropped_sequence = number;
      counters.last_dropped_sequence = number;
      return;
    }
    try {
      if (notifications.size() < config.max_pending_notifications) {
        notifications.push_back({number, type, entry.handle,
                                 type == RuntimeNotificationType::Started
                                     ? SessionOutcome::Started : entry.outcome});
        ++enqueued;
        counters.notification_high_watermark = std::max(
            counters.notification_high_watermark, notifications.size());
        cv.notify_all();
        return;
      }
    } catch (...) {
      // Notification allocation failure must not roll back an accepted command.
    }
    ++counters.dropped_notifications;
    if (!counters.first_dropped_sequence) counters.first_dropped_sequence = number;
    counters.last_dropped_sequence = number;
  }

  std::shared_ptr<Entry> matching(const SessionHandle& handle) const {
    if (handle.runtime_id != runtime_id) return {};
    auto it = registry.find(handle.id);
    if (it == registry.end() || it->second->handle.generation != handle.generation)
      return {};
    return it->second;
  }

  void stop_entry(Entry& entry, SessionOutcome reason) {
    if (entry.phase == SessionPhase::Retiring) return;
    const bool first = !entry.stop_requested;
    // A later fatal error overrides a normal removal, but not another fatal cause.
    const bool escalation = failed(reason) && !failed(entry.outcome);
    if (!first && !escalation) return;
    entry.outcome = reason;
    if (first && entry.phase == SessionPhase::Reserved) {
      // Retire cancelled reservations even when every startup worker is stuck.
      // The registry still owns the factory; its captures are released by the
      // supervisor outside this lock, before either completion is reported.
      auto queued = std::find_if(pending.begin(), pending.end(), [&](const auto& e) {
        return e.get() == &entry;
      });
      if (queued != pending.end()) pending.erase(queued);
      entry.cancelled_before_start = true;
      entry.startup_done = true;
    }
    entry.stop_requested = true;
    entry.phase = SessionPhase::StopRequested;
    // Internal non-waiting flag update only: no engine or user code is invoked.
    // Lock ordering is registry -> session, never the inverse.
    if (entry.session) entry.session->request_stop();
    if (first) publish(entry, RuntimeNotificationType::StopRequested);
    changed();
  }

  void record_rejection(AdmissionStatus status) {
    ++counters.rejected;
    switch (status) {
      case AdmissionStatus::InvalidRequest: ++counters.rejected_invalid; break;
      case AdmissionStatus::DuplicateId: ++counters.rejected_duplicate; break;
      case AdmissionStatus::CapacityReached: ++counters.rejected_capacity; break;
      case AdmissionStatus::StartupQueueFull: ++counters.rejected_queue; break;
      case AdmissionStatus::Draining: ++counters.rejected_draining; break;
      case AdmissionStatus::IdentityExhausted: ++counters.rejected_identity; break;
      case AdmissionStatus::Accepted: break;
    }
  }

  SessionAdmission add(std::string id, SessionFactory factory) {
    SessionAdmission result;
    if (id.empty() || id.size() > config.max_session_id_bytes || !factory) {
      std::lock_guard<std::mutex> lock(mutex);
      record_rejection(result.status);
      return result;
    }
    // Allocation may throw before acceptance. The factory is not invoked here.
    auto entry = std::make_shared<Entry>();
    entry->handle = {runtime_id, std::move(id), 0};
    entry->factory = std::move(factory);
    auto started = entry->started.get_future().share();
    auto retired = entry->retired.get_future().share();
    std::lock_guard<std::mutex> lock(mutex);
    if (counters.lifecycle != RuntimeLifecycle::Running)
      result.status = AdmissionStatus::Draining;
    else if (registry.count(entry->handle.id)) result.status = AdmissionStatus::DuplicateId;
    else if (registry.size() >= config.max_sessions)
      result.status = AdmissionStatus::CapacityReached;
    else if (pending.size() >= config.max_pending_starts)
      result.status = AdmissionStatus::StartupQueueFull;
    else if (generation == std::numeric_limits<std::uint64_t>::max())
      result.status = AdmissionStatus::IdentityExhausted;
    else {
      entry->handle.generation = generation++;
      // Prepare the result before mutating registry/queue (string copies allocate).
      result = {AdmissionStatus::Accepted, entry->handle, started, retired};
      registry.emplace(entry->handle.id, entry);
      try { pending.push_back(entry); }
      catch (...) { registry.erase(entry->handle.id); throw; }
      ++counters.admitted;
      publish(*entry, RuntimeNotificationType::Reserved);
      changed();
      return result;
    }
    record_rejection(result.status);
    return result;
    // lock is destroyed before entry/factory; captures may call the runtime.
  }

  RemoveStatus remove(const SessionHandle& handle, SessionOutcome reason) {
    std::lock_guard<std::mutex> lock(mutex);
    if (handle.runtime_id != runtime_id) return RemoveStatus::StaleHandle;
    auto it = registry.find(handle.id);
    if (it == registry.end()) return RemoveStatus::NotFound;
    auto& entry = *it->second;
    if (handle.generation != entry.handle.generation) return RemoveStatus::StaleHandle;
    const bool already = entry.stop_requested;
    stop_entry(entry, reason);
    return already ? RemoveStatus::AlreadyRequested : RemoveStatus::Requested;
  }

  struct Operation {
    Impl& owner;
    RuntimeCall call;
    std::shared_ptr<Entry> entry;
    Operation(Impl& value, const SessionHandle& handle) : owner(value), call(&value) {
      std::lock_guard<std::mutex> lock(owner.mutex);
      auto candidate = owner.matching(handle);
      if (owner.counters.lifecycle == RuntimeLifecycle::Running && candidate &&
          candidate->phase == SessionPhase::Running && !candidate->stop_requested) {
        entry = std::move(candidate);
        ++entry->operations;
      }
    }
    ~Operation() {
      if (!entry) return;
      std::lock_guard<std::mutex> lock(owner.mutex);
      --entry->operations;
      if (entry->stop_requested) owner.changed();
    }
  };

  void startup_loop() {
    RuntimeCall call(this);
    while (true) {
      std::shared_ptr<Entry> entry;
      SessionFactory factory;
      bool attempt = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return abort_construction || !pending.empty() ||
                                      counters.lifecycle != RuntimeLifecycle::Running; });
        if (abort_construction) return;
        if (pending.empty()) { ++workers_done; changed(); return; }
        entry = std::move(pending.front());
        pending.pop_front();
        attempt = !entry->stop_requested;
        if (attempt) entry->phase = SessionPhase::Starting;
        factory = std::move(entry->factory);
      }
      bool startup_failed = false;
      if (attempt) {
        try {
          auto resources = factory(entry->handle);
          resources.timeline.generation = entry->handle.generation;
          auto session = std::make_unique<ConversationSession>(
              entry->handle.id, std::move(resources.engine), nullptr,
              std::move(resources.timeline));
          ConversationSession* instance = session.get();
          bool start = false;
          {
            std::lock_guard<std::mutex> lock(mutex);
            entry->dependencies = std::move(resources.dependencies);
            entry->session = std::move(session);
            start = !entry->stop_requested;
            if (!start) instance->request_stop();
          }
          if (start) instance->start();
        } catch (...) {
          // Cancellation racing start is not itself a startup failure.
          std::lock_guard<std::mutex> lock(mutex);
          startup_failed = !entry->stop_requested || !entry->session ||
                           static_cast<bool>(entry->session->failure());
        }
      }
      factory = {};  // Release user captures outside the registry lock.
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (startup_failed) stop_entry(*entry, SessionOutcome::StartupFailed);
        entry->startup_done = true;
        if (!entry->stop_requested) {
          entry->phase = SessionPhase::Running;
          entry->started.set_value(SessionOutcome::Started);
          publish(*entry, RuntimeNotificationType::Started);
        } else {
          entry->started.set_value(entry->outcome);
        }
        changed();
      }
    }
  }

  void supervise() {
    RuntimeCall call(this);
    auto& entries = scan_entries;
    while (true) {
      entries.clear();
      std::uint64_t observed;
      {
        std::unique_lock<std::mutex> lock(mutex);
        if (abort_construction) return;
        if (registry.empty() && counters.lifecycle != RuntimeLifecycle::Running &&
            workers_done == config.startup_workers) {
          supervisor_done = true;
          cv.notify_all();
          return;
        }
        observed = revision;
        for (auto& item : registry) entries.push_back(item.second);
      }
      for (auto& entry : entries) {
        ConversationSession* session = nullptr;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!entry->startup_done) continue;
          session = entry->session.get();
        }
        if (session) {
          auto state = session->lifecycle();
          if (!terminal(state)) continue;
          {
            std::lock_guard<std::mutex> lock(mutex);
            if (state == SessionLifecycle::Failed)
              stop_entry(*entry, SessionOutcome::LifecycleFailure);
            else if (!entry->stop_requested)
              stop_entry(*entry, SessionOutcome::UnexpectedStop);
          }
          // A blocked session observer must not block the supervisor scan.
          // Closed timeline + zero queued/in-flight callbacks precedes destruction.
          if (!session->timeline().flush(std::chrono::milliseconds(0))) continue;
        }
        std::unique_ptr<ConversationSession> owned;
        std::shared_ptr<void> dependencies;
        SessionFactory abandoned_factory;
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (entry->operations) continue;
          entry->phase = SessionPhase::Retiring;
          owned = std::move(entry->session);
          dependencies = std::move(entry->dependencies);
          abandoned_factory = std::move(entry->factory);
        }
        owned.reset();
        dependencies.reset();  // Engine/session teardown precedes dependencies.
        abandoned_factory = {};
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++counters.retired;
          if (failed(entry->outcome)) ++counters.failures;
          registry.erase(entry->handle.id);
          if (entry->cancelled_before_start) entry->started.set_value(entry->outcome);
          publish(*entry, RuntimeNotificationType::Retired);
          entry->retired.set_value(entry->outcome);
          changed();
        }
      }
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, config.supervisor_interval, [this, observed] {
          return abort_construction || revision != observed;
        });
      }
    }
  }

  void dispatch() {
    RuntimeCall call(this);
    while (true) {
      RuntimeNotification notification;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
          return abort_construction || supervisor_done || !notifications.empty();
        });
        if (abort_construction) return;
        if (notifications.empty() && supervisor_done) {
          counters.lifecycle = RuntimeLifecycle::Stopped;
          cv.notify_all();
          return;
        }
        notification = std::move(notifications.front());
        notifications.pop_front();
      }
      bool error = false;
      try { notify(notification); } catch (...) { error = true; }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (error) ++counters.observer_failures;
        ++delivered;
        cv.notify_all();
      }
    }
  }

  void request_shutdown() noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    if (counters.lifecycle != RuntimeLifecycle::Running) return;
    counters.lifecycle = RuntimeLifecycle::Draining;
    for (auto& item : registry) stop_entry(*item.second, SessionOutcome::RuntimeShutdown);
    changed();
  }
  bool wait_shutdown(std::chrono::milliseconds timeout) {
    if (callback()) return false;
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, timeout, [this] {
      return counters.lifecycle == RuntimeLifecycle::Stopped;
    });
  }
  void join() noexcept {
    std::lock_guard<std::mutex> lock(join_mutex);
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    if (supervisor.joinable()) supervisor.join();
    if (dispatcher.joinable()) dispatcher.join();
  }
  void shutdown() noexcept {
    request_shutdown();
    if (callback()) return;
    join();
  }
};

RuntimeManager::RuntimeManager(RuntimeConfig config, NotifySink notify)
    : impl_(std::make_unique<Impl>(config, std::move(notify))) {}
RuntimeManager::~RuntimeManager() { impl_->shutdown(); }
SessionAdmission RuntimeManager::add(std::string id, SessionFactory factory) {
  return impl_->add(std::move(id), std::move(factory));
}
RemoveStatus RuntimeManager::remove(const SessionHandle& handle) {
  return impl_->remove(handle, SessionOutcome::Removed);
}
RemoveStatus RuntimeManager::fail(const SessionHandle& handle) {
  return impl_->remove(handle, SessionOutcome::ReportedFailure);
}
bool RuntimeManager::push_audio(const SessionHandle& handle, AudioFrame frame) {
  Impl::Operation op(*impl_, handle);
  if (!op.entry) return false;
  try { return op.entry->session->push_audio(std::move(frame)); }
  catch (...) { impl_->remove(handle, SessionOutcome::EngineFailure); return false; }
}
bool RuntimeManager::handle_event(const SessionHandle& handle, Event event) {
  Impl::Operation op(*impl_, handle);
  if (!op.entry) return false;
  if ((!event.session_id.empty() && event.session_id != handle.id) ||
      (event.generation && event.generation != handle.generation)) return false;
  try { op.entry->session->handle_event(std::move(event)); return true; }
  catch (...) { impl_->remove(handle, SessionOutcome::EngineFailure); return false; }
}
TimelineAppendResult RuntimeManager::observe(const SessionHandle& handle, Event event) {
  Impl::Operation op(*impl_, handle);
  if (!op.entry) return {};
  return op.entry->session->observe(std::move(event));
}
std::optional<SessionInfo> RuntimeManager::find(const SessionHandle& handle) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto entry = impl_->matching(handle);
  if (!entry) return {};
  return SessionInfo{entry->handle, entry->phase, entry->outcome, entry->operations};
}
std::vector<SessionInfo> RuntimeManager::snapshot() const {
  std::vector<SessionInfo> result;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  result.reserve(impl_->registry.size());
  for (auto& item : impl_->registry) {
    auto& e = *item.second;
    result.push_back({e.handle, e.phase, e.outcome, e.operations});
  }
  return result;
}
RuntimeStats RuntimeManager::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  auto result = impl_->counters;
  result.sessions = impl_->registry.size();
  result.pending_starts = impl_->pending.size();
  result.pending_notifications = impl_->notifications.size();
  for (auto& item : impl_->registry) {
    switch (item.second->phase) {
      case SessionPhase::Reserved: ++result.reserved; break;
      case SessionPhase::Starting: ++result.starting; break;
      case SessionPhase::Running: ++result.running; break;
      case SessionPhase::StopRequested: ++result.stopping; break;
      case SessionPhase::Retiring: ++result.retiring; break;
    }
  }
  return result;
}
bool RuntimeManager::flush_notifications(std::chrono::milliseconds timeout) {
  if (impl_->callback()) return false;
  std::unique_lock<std::mutex> lock(impl_->mutex);
  const auto target = impl_->enqueued;
  return impl_->cv.wait_for(lock, timeout, [this, target] {
    return impl_->delivered >= target;
  });
}
void RuntimeManager::request_shutdown() noexcept { impl_->request_shutdown(); }
bool RuntimeManager::wait_shutdown(std::chrono::milliseconds timeout) {
  return impl_->wait_shutdown(timeout);
}
void RuntimeManager::shutdown() noexcept { impl_->shutdown(); }

}  // namespace byteturn
