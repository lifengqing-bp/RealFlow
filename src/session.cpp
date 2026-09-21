#include "byteturn/session.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

AsyncSession::AsyncSession(std::string session_id, Agent& agent,
                           SessionExecutor& executor, EventBus* events)
    : session_id_(std::move(session_id)), agent_(agent), executor_(executor),
      events_(events) {
  if (session_id_.empty()) throw std::invalid_argument("session id is required");
}

TurnHandle AsyncSession::submit(std::string user_text) {
  return executor_.submit(
      session_id_, [this, text = std::move(user_text)](
                       const CancellationToken& token,
                       const std::string& turn_id) mutable {
        if (token.cancelled()) throw std::runtime_error("turn cancelled");
        std::string reply = agent_.run(std::move(text), session_id_, turn_id);
        if (token.cancelled()) {
          if (events_) events_->publish(
              {EventType::TurnCancelled, session_id_, turn_id, 0, {}, {}, {}});
          throw std::runtime_error("turn cancelled");
        }
        return reply;
      });
}

void AsyncSession::cancel() { executor_.cancel_session(session_id_); }

}  // namespace byteturn
