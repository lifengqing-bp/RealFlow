#pragma once

#include "byteturn/conversation_session.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace byteturn {

// Process-local identity. IDs may be reused only after retirement; generations
// and runtime_id prevent late callbacks from operating on a replacement.
struct SessionHandle {
  std::uint64_t runtime_id = 0;
  std::string id;
  std::uint64_t generation = 0;
};

enum class RuntimeLifecycle { Running, Draining, Stopped };
enum class SessionPhase { Reserved, Starting, Running, StopRequested, Retiring };
enum class AdmissionStatus {
  Accepted, InvalidRequest, DuplicateId, CapacityReached, StartupQueueFull,
  Draining, IdentityExhausted
};
enum class RemoveStatus { Requested, AlreadyRequested, NotFound, StaleHandle };
enum class SessionOutcome {
  None, Started, Removed, RuntimeShutdown, StartupFailed, EngineFailure,
  ReportedFailure, LifecycleFailure, UnexpectedStop
};

struct SessionAdmission {
  AdmissionStatus status = AdmissionStatus::InvalidRequest;
  SessionHandle handle;
  // Accepted is a reservation, not proof of startup. Neither future requires
  // an observer. retired becomes ready only after resource teardown and erase.
  std::shared_future<SessionOutcome> started;
  std::shared_future<SessionOutcome> retired;
  explicit operator bool() const { return status == AdmissionStatus::Accepted; }
};

// Keep dependencies alive through engine destruction. Factories borrowing
// binary-owned objects may omit dependencies, but those objects must outlive
// the manager. The member order also protects cleanup after construction fails.
struct SessionResources {
  std::shared_ptr<void> dependencies;
  std::unique_ptr<ConversationEngine> engine;
  TimelineConfig timeline;
};

struct RuntimeConfig {
  std::size_t max_sessions = 64;
  std::size_t max_pending_starts = 16;
  std::size_t startup_workers = 2;
  std::size_t max_pending_notifications = 256;
  std::size_t max_session_id_bytes = 256;
  std::chrono::milliseconds supervisor_interval{5};
};

struct SessionInfo {
  SessionHandle handle;
  SessionPhase phase = SessionPhase::Reserved;
  SessionOutcome outcome = SessionOutcome::None;
  std::size_t admitted_operations = 0;
};

enum class RuntimeNotificationType { Reserved, Started, StopRequested, Retired };
struct RuntimeNotification {
  std::uint64_t sequence = 0;
  RuntimeNotificationType type = RuntimeNotificationType::Reserved;
  SessionHandle handle;
  SessionOutcome outcome = SessionOutcome::None;
};

struct RuntimeStats {
  RuntimeLifecycle lifecycle = RuntimeLifecycle::Running;
  // All phases count against max_sessions, including Retiring.
  std::size_t sessions = 0;
  std::size_t reserved = 0, starting = 0, running = 0, stopping = 0, retiring = 0;
  std::size_t pending_starts = 0;
  std::uint64_t admitted = 0, rejected = 0, retired = 0, failures = 0;
  std::uint64_t rejected_invalid = 0, rejected_duplicate = 0;
  std::uint64_t rejected_capacity = 0, rejected_queue = 0;
  std::uint64_t rejected_draining = 0, rejected_identity = 0;
  std::size_t pending_notifications = 0, notification_high_watermark = 0;
  std::uint64_t dropped_notifications = 0, observer_failures = 0;
  std::uint64_t first_dropped_sequence = 0, last_dropped_sequence = 0;
};

// Binary-owned registry and stop-and-retire supervisor. Does not install signal
// handlers, restart sessions, terminate threads, or provide process isolation.
// All methods are thread-safe; factory/engine calls and observers run outside
// the registry mutex. Never destroy the manager from any of its callbacks.
class RuntimeManager {
 public:
  using SessionFactory = std::function<SessionResources(const SessionHandle&)>;
  using NotifySink = std::function<void(const RuntimeNotification&)>;

  explicit RuntimeManager(RuntimeConfig config = {}, NotifySink notify = {});
  ~RuntimeManager();
  RuntimeManager(const RuntimeManager&) = delete;
  RuntimeManager& operator=(const RuntimeManager&) = delete;

  SessionAdmission add(std::string id, SessionFactory factory);
  RemoveStatus remove(const SessionHandle& handle);
  // Explicit fatal report; ordinary Error observations do not call this.
  RemoveStatus fail(const SessionHandle& handle);
  bool push_audio(const SessionHandle& handle, AudioFrame frame);
  // True means the legacy hook was forwarded, not that a typed command was
  // accepted. Use observe() for explicit timeline admission results.
  bool handle_event(const SessionHandle& handle, Event event);
  TimelineAppendResult observe(const SessionHandle& handle, Event event);

  std::optional<SessionInfo> find(const SessionHandle& handle) const;
  std::vector<SessionInfo> snapshot() const;
  RuntimeStats stats() const;
  bool flush_notifications(std::chrono::milliseconds timeout);

  void request_shutdown() noexcept;
  // Known runtime/EventBus callbacks cannot wait. Arbitrary provider callbacks
  // must use request_shutdown(); they cannot all be detected by this library.
  bool wait_shutdown(std::chrono::milliseconds timeout);
  void shutdown() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace byteturn
