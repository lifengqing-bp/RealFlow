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

TurnHandle AsyncSession::submit(std::string user_text, TurnOptions options) {
  return submit_streaming(std::move(user_text),
                          [](const std::string&) { return true; }, {}, {},
                          std::move(options));
}

TurnHandle AsyncSession::submit_streaming(
    std::string user_text, LlmProvider::TextDeltaSink on_text_delta,
    Completion on_completed, Failure on_failed, TurnOptions options) {
  return executor_.submit_turn(
      session_id_, std::move(options),
      [this, text = std::move(user_text),
       on_text_delta = std::move(on_text_delta),
       on_completed = std::move(on_completed),
       on_failed = std::move(on_failed)](const TurnContext& context) mutable {
        try {
          if (context.stop_requested())
            throw std::runtime_error(context.expired() ? "turn deadline exceeded"
                                                       : "turn cancelled");
          context.transition(TurnState::ModelStreaming);
          std::string reply = agent_.run_streaming(
              std::move(text), session_id_, context.turn_id(),
              on_text_delta,
              [&context] { return context.stop_requested(); },
              context.trace_id());
          if (context.stop_requested())
            throw std::runtime_error(context.expired() ? "turn deadline exceeded"
                                                       : "turn cancelled");
          if (on_completed) on_completed(context, reply);
          return reply;
        } catch (...) {
          const auto error = std::current_exception();
          if (on_failed) on_failed(context, error);
          if (events_ && context.stop_requested()) {
            events_->publish(
                {context.expired() ? EventType::Error : EventType::TurnCancelled,
                 session_id_, context.turn_id(), 0, {},
                 context.expired() ? "deadline" : "cancelled", {}});
          }
          std::rethrow_exception(error);
        }
      });
}

void AsyncSession::cancel() { executor_.cancel_session(session_id_); }

}  // namespace byteturn
