#include "byteturn/conversation_session.h"

#include <utility>

namespace byteturn {

ConversationSession::ConversationSession(
    std::string session_id, std::unique_ptr<ConversationEngine> engine,
    EventBus* events)
    : session_id_(std::move(session_id)),
      timeline_(events),
      engine_(std::move(engine)) {
  if (session_id_.empty()) throw std::invalid_argument("session id is required");
  if (!engine_) throw std::invalid_argument("conversation engine is required");
}

ConversationSession::~ConversationSession() { stop(); }

void ConversationSession::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return;
  if (stopped_) throw std::logic_error("conversation session already stopped");
  engine_->start({session_id_, &timeline_});
  started_ = true;
}

bool ConversationSession::push_audio(AudioFrame frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopped_) return false;
  }
  return engine_->push_audio(std::move(frame));
}

void ConversationSession::handle_event(Event event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopped_) return;
  }
  if (event.session_id.empty()) event.session_id = session_id_;
  timeline_.append(event);
  engine_->handle_event(event);
}

void ConversationSession::stop() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!started_ || stopped_) return;
  stopped_ = true;
  lock.unlock();
  engine_->stop();
}

ConversationCapabilities ConversationSession::capabilities() const {
  return engine_->capabilities();
}

ConversationStateSnapshot ConversationSession::state() const {
  return engine_->state();
}

}  // namespace byteturn
