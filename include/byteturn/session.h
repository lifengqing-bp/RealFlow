#pragma once

#include "byteturn/agent.h"
#include "byteturn/executor.h"

#include <string>

namespace byteturn {

// Binds one Agent's mutable history to one serialized executor lane.
// The Agent and executor must outlive this object and all returned futures.
class AsyncSession {
 public:
  AsyncSession(std::string session_id, Agent& agent, SessionExecutor& executor,
               EventBus* events = nullptr);

  TurnHandle submit(std::string user_text);
  void cancel();
  const std::string& id() const { return session_id_; }

 private:
  std::string session_id_;
  Agent& agent_;
  SessionExecutor& executor_;
  EventBus* events_;
};

}  // namespace byteturn

