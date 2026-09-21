#include "byteturn/conversation_session.h"

#include <stdexcept>
#include <utility>

namespace byteturn {
namespace {
struct SessionCall {
  explicit SessionCall(const void* owner);
  ~SessionCall();
  const void* session;
  SessionCall* previous;
};
thread_local SessionCall* active_session_call = nullptr;
SessionCall::SessionCall(const void* owner)
    : session(owner), previous(active_session_call) {
  active_session_call = this;
}
SessionCall::~SessionCall() { active_session_call = previous; }

bool terminal(SessionLifecycle state) {
  return state == SessionLifecycle::Stopped || state == SessionLifecycle::Failed;
}

std::string checked_id(std::string id) {
  if (id.empty()) throw std::invalid_argument("session id is required");
  return id;
}

TimelineConfig scoped_config(TimelineConfig config, const std::string& id) {
  config.session_id = id;
  return config;
}
}  // namespace

class ConversationSession::Operation {
 public:
  explicit Operation(const ConversationSession& owner)
      : owner_(owner), admitted_(owner.acquire_operation()), call_(&owner) {}
  ~Operation() { if (admitted_) owner_.release_operation(); }
  explicit operator bool() const { return admitted_; }
 private:
  const ConversationSession& owner_;
  bool admitted_;
  SessionCall call_;
};

ConversationSession::ConversationSession(
    std::string session_id, std::unique_ptr<ConversationEngine> engine,
    EventBus* events, TimelineConfig timeline_config)
    : session_id_(checked_id(std::move(session_id))),
      generation_(timeline_config.generation),
      timeline_(events, scoped_config(std::move(timeline_config), session_id_)),
      engine_(std::move(engine)) {
  if (!engine_) throw std::invalid_argument("conversation engine is required");
  lifecycle_worker_ = std::thread([this] { lifecycle_loop(); });
}

ConversationSession::~ConversationSession() {
  // The owner must never destroy this object from an engine/observer callback.
  // All callbacks and borrowed dependencies remain alive until these joins end.
  stop();
  if (lifecycle_worker_.joinable()) lifecycle_worker_.join();
  timeline_.shutdown();
}

bool ConversationSession::callback_thread() const noexcept {
  if (EventBus::in_callback() || timeline_.is_dispatch_thread()) return true;
  for (auto* frame = active_session_call; frame; frame = frame->previous)
    if (frame->session == this) return true;
  return false;
}

void ConversationSession::start() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (lifecycle_ == SessionLifecycle::Running) return;
  if (callback_thread())
    throw std::logic_error("start cannot wait inside a session callback");
  if (lifecycle_ != SessionLifecycle::Created &&
      lifecycle_ != SessionLifecycle::Starting)
    throw std::logic_error("conversation session cannot restart");
  if (lifecycle_ == SessionLifecycle::Created) {
    lifecycle_ = SessionLifecycle::Starting;
    start_requested_ = true;
    cv_.notify_all();
  }
  cv_.wait(lock, [this] {
    return lifecycle_ == SessionLifecycle::Running || terminal(lifecycle_);
  });
  if (failure_) std::rethrow_exception(failure_);
  if (lifecycle_ != SessionLifecycle::Running)
    throw std::runtime_error("conversation startup was stopped");
}

bool ConversationSession::acquire_operation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lifecycle_ != SessionLifecycle::Running) return false;
  ++active_operations_;
  return true;
}

void ConversationSession::release_operation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  --active_operations_;
  cv_.notify_all();
}

bool ConversationSession::push_audio(AudioFrame frame) {
  Operation operation(*this);
  return operation && engine_->push_audio(std::move(frame));
}

bool ConversationSession::cancel_response() {
  Operation operation(*this);
  return operation && engine_->cancel_response();
}

TimelineAppendResult ConversationSession::observe(Event event) {
  Operation operation(*this);
  if (!operation) return {};
  return timeline_.append(std::move(event));
}

void ConversationSession::handle_event(Event event) {
  Operation operation(*this);
  if (!operation) return;
  Event normalized(EventType::Error);
  if (timeline_.append(std::move(event), &normalized))
    engine_->handle_event(normalized);
}

void ConversationSession::request_stop() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal(lifecycle_)) return;
  stop_requested_ = true;
  lifecycle_ = SessionLifecycle::Stopping;
  cv_.notify_all();
}

void ConversationSession::stop() noexcept {
  request_stop();
  if (callback_thread()) return;
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return terminal(lifecycle_); });
}

bool ConversationSession::wait_stopped(std::chrono::milliseconds timeout) {
  if (callback_thread()) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [this] { return terminal(lifecycle_); });
}

SessionLifecycle ConversationSession::lifecycle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lifecycle_;
}

std::exception_ptr ConversationSession::failure() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failure_;
}

ConversationCapabilities ConversationSession::capabilities() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capabilities_;
}

ConversationStateSnapshot ConversationSession::state() const {
  Operation operation(*this);
  return operation ? engine_->state() : ConversationStateSnapshot{};
}

void ConversationSession::publish_lifecycle(EventType type,
                                             const char* phase) noexcept {
  try {
    (void)timeline_.append({type, session_id_, {}, 0, {}, phase});
  } catch (...) {
    // Shutdown must still quiesce engine resources if recording fails.
  }
}

void ConversationSession::lifecycle_loop() noexcept {
  SessionCall call(this);
  bool attempted_start = false;
  std::exception_ptr failure;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return start_requested_ || stop_requested_; });
    attempted_start = start_requested_ && !stop_requested_;
  }

  if (attempted_start) {
    try {
      auto caps = engine_->capabilities();
      ConversationEngineContext context;
      context.session_id = session_id_;
      context.timeline = &timeline_;
      context.emit = timeline_.sink();
      // The configured generation is normalized by the sink; obtain it without
      // inventing a second independent sequence or generation allocator.
      context.generation = generation_;
      engine_->start(std::move(context));
      publish_lifecycle(EventType::SessionStarted, "started");
      std::lock_guard<std::mutex> lock(mutex_);
      capabilities_ = caps;
      if (!stop_requested_) lifecycle_ = SessionLifecycle::Running;
      cv_.notify_all();
    } catch (...) {
      failure = std::current_exception();
      std::lock_guard<std::mutex> lock(mutex_);
      stop_requested_ = true;
      lifecycle_ = SessionLifecycle::Stopping;
    }
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return stop_requested_; });
    cv_.wait(lock, [this] { return active_operations_ == 0; });
  }
  if (attempted_start) {
    try {
      engine_->stop();  // Also cleans up a partially failed start, exactly once.
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  publish_lifecycle(failure ? EventType::SessionFailed : EventType::SessionStopped,
                    failure ? "lifecycle_failure" : "stopped");
  timeline_.close();  // Fences weak sinks and legacy ingress after cleanup.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failure_ = failure;
    lifecycle_ = failure ? SessionLifecycle::Failed : SessionLifecycle::Stopped;
    cv_.notify_all();
  }
}

}  // namespace byteturn
