#pragma once

#include "byteturn/agent.h"
#include "byteturn/executor.h"
#include "byteturn/turn_context.h"

#include <string>
#include <exception>

namespace byteturn {

// Binds one Agent's mutable history to one serialized executor lane.
// The Agent and executor must outlive this object and all returned futures.
class AsyncSession {
 public:
  using Completion =
      std::function<void(const TurnContext&, const std::string& reply)>;
  using Failure = std::function<void(const TurnContext&, std::exception_ptr)>;
  AsyncSession(std::string session_id, Agent& agent, SessionExecutor& executor,
               EventBus* events = nullptr);

  TurnHandle submit(std::string user_text, TurnOptions options = {});
  TurnHandle submit_streaming(std::string user_text,
                              LlmProvider::TextDeltaSink on_text_delta,
                              Completion on_completed = {},
                              Failure on_failed = {},
                              TurnOptions options = {});
  void cancel();
  const std::string& id() const { return session_id_; }

 private:
  std::string session_id_;
  Agent& agent_;
  SessionExecutor& executor_;
  EventBus* events_;
};

}  // namespace byteturn
