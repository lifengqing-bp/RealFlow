#pragma once

#include "byteturn/conversation_engine.h"

#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace byteturn {

enum class SessionLifecycle { Created, Starting, Running, Stopping, Stopped, Failed };

// Owns one non-restartable interaction lifetime. Lifecycle work is isolated
// from media admission; shutdown closes admission then drains accepted calls.
// Borrowed providers, legacy sessions and an external bus must outlive this
// object. Destruction must occur on an owning thread, outside all callbacks.
class ConversationSession {
 public:
  ConversationSession(std::string session_id,
                      std::unique_ptr<ConversationEngine> engine,
                      EventBus* events = nullptr,
                      TimelineConfig timeline_config = {});
  ~ConversationSession();

  ConversationSession(const ConversationSession&) = delete;
  ConversationSession& operator=(const ConversationSession&) = delete;

  // Concurrent callers share one startup. A failure is rethrown only after
  // cleanup; neither a failed nor stopped session can restart.
  void start();
  bool push_audio(AudioFrame frame);

  // Observation-only ingress: never asks an engine to perform an action.
  TimelineAppendResult observe(Event event);
  // Compatibility ingress. Normalizes/validates before forwarding to the
  // legacy engine hook. Typed commands are a subsequent adapter milestone.
  void handle_event(Event event);

  void request_stop() noexcept;  // Safe, non-waiting request from any callback.
  // Quiescent on external threads. On an admitted engine call, lifecycle lane,
  // or EventBus callback, only requests stop (otherwise it would self-wait).
  void stop() noexcept;
  // False on timeout or callback re-entry; no hidden join of the caller.
  bool wait_stopped(std::chrono::milliseconds timeout);

  const std::string& id() const { return session_id_; }
  EventTimeline& timeline() { return timeline_; }
  const EventTimeline& timeline() const { return timeline_; }
  SessionLifecycle lifecycle() const;
  std::exception_ptr failure() const;
  ConversationCapabilities capabilities() const;
  ConversationStateSnapshot state() const;

 private:
  class Operation;
  bool callback_thread() const noexcept;
  bool acquire_operation() const;
  void release_operation() const;
  void lifecycle_loop() noexcept;
  void publish_lifecycle(EventType type, const char* phase) noexcept;

  std::string session_id_;
  const std::uint64_t generation_;
  EventTimeline timeline_;
  std::unique_ptr<ConversationEngine> engine_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable std::size_t active_operations_ = 0;
  SessionLifecycle lifecycle_ = SessionLifecycle::Created;
  bool start_requested_ = false;
  bool stop_requested_ = false;
  std::exception_ptr failure_;
  ConversationCapabilities capabilities_;
  std::thread lifecycle_worker_;
};

}  // namespace byteturn
