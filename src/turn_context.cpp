#include "byteturn/turn_context.h"

#include <utility>

namespace byteturn {

TurnContext::TurnContext(std::string session_id, std::string turn_id,
                         std::string trace_id, CancellationToken cancellation,
                         std::chrono::steady_clock::time_point deadline)
    : session_id_(std::move(session_id)), turn_id_(std::move(turn_id)),
      trace_id_(std::move(trace_id)), cancellation_(std::move(cancellation)),
      deadline_(deadline) {}

std::chrono::milliseconds TurnContext::remaining() const {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline_) return std::chrono::milliseconds(0);
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
}

}  // namespace byteturn
