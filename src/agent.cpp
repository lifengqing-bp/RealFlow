#include "byteturn/agent.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

bool ToolRegistry::add(std::string name, ToolHandler handler) {
  return add(std::move(name),
             [handler = std::move(handler)](const std::string& arguments,
                                            const std::function<bool()>&) {
               return handler(arguments);
             });
}

bool ToolRegistry::add(std::string name, CancellableToolHandler handler) {
  return tools_.emplace(std::move(name), std::move(handler)).second;
}

std::string ToolRegistry::invoke(const std::string& name,
                                 const std::string& arguments) const {
  return invoke(name, arguments, {});
}

std::string ToolRegistry::invoke(
    const std::string& name, const std::string& arguments,
    const std::function<bool()>& stop_requested) const {
  const auto it = tools_.find(name);
  if (it == tools_.end()) return "error: unknown tool: " + name;
  if (stop_requested && stop_requested())
    throw std::runtime_error("tool cancelled before execution");
  try {
    std::string result = it->second(arguments, stop_requested);
    if (stop_requested && stop_requested())
      throw std::runtime_error("tool cancelled during execution");
    return result;
  } catch (const std::exception& e) {
    if (stop_requested && stop_requested()) throw;
    return "error: tool failed: " + std::string(e.what());
  }
}

Agent::Agent(LlmProvider& llm, ToolRegistry& tools, std::size_t max_steps,
             EventBus* events)
    : llm_(llm), tools_(tools), max_steps_(max_steps), events_(events) {
  history_.push_back({Role::System,
                      "You are ByteTurn, a concise voice agent. Use tools when useful.",
                      {}, {}, {}});
}

std::string Agent::run(std::string user_text) {
  return run(std::move(user_text), {}, {});
}

std::string Agent::run(std::string user_text, std::string session_id,
                       std::string turn_id, std::function<bool()> cancelled) {
  return run_streaming(std::move(user_text), std::move(session_id),
                       std::move(turn_id), [](const std::string&) { return true; },
                       std::move(cancelled), {});
}

std::string Agent::run_streaming(std::string user_text, std::string session_id,
                                 std::string turn_id,
                                 LlmProvider::TextDeltaSink on_text_delta,
                                 std::function<bool()> cancelled,
                                 std::string trace_id) {
  const auto emit = [&](EventType type, std::string name = {},
                        std::string data = {}) {
    if (events_) events_->publish(
        {type, session_id, turn_id, 0, {}, std::move(name), std::move(data),
         trace_id});
  };
  emit(EventType::TurnStarted);
  const auto history_checkpoint = history_.size();
  if (cancelled && cancelled()) throw std::runtime_error("turn cancelled");
  history_.push_back({Role::User, std::move(user_text), {}, {}, {}});
  for (std::size_t step = 0; step < max_steps_; ++step) {
    if (cancelled && cancelled()) {
      history_.resize(history_checkpoint);
      throw std::runtime_error("turn cancelled");
    }
    emit(EventType::ModelStarted, std::to_string(step));
    LlmTurn turn;
    bool received_first_token = false;
    try {
      turn = llm_.stream(
          history_,
          [&](const std::string& delta) {
            if (cancelled && cancelled()) return false;
            if (delta.empty()) return true;
            if (!received_first_token) {
              received_first_token = true;
              emit(EventType::FirstToken, std::to_string(step));
            }
            emit(EventType::ModelTextDelta, std::to_string(step), delta);
            return on_text_delta(delta);
          },
          cancelled);
    } catch (const std::exception& e) {
      history_.resize(history_checkpoint);
      emit(EventType::Error, "llm", e.what());
      throw;
    }
    if (cancelled && cancelled()) {
      history_.resize(history_checkpoint);
      throw std::runtime_error("turn cancelled");
    }
    emit(EventType::ModelCompleted, std::to_string(step), turn.text);
    history_.push_back({Role::Assistant, turn.text, {}, turn.tool_calls, {}});
    if (turn.tool_calls.empty()) {
      emit(EventType::TurnCompleted, {}, turn.text);
      return turn.text;
    }

    for (const auto& call : turn.tool_calls) {
      emit(EventType::ToolStarted, call.name, call.arguments);
      std::string result;
      try {
        result = tools_.invoke(call.name, call.arguments, cancelled);
      } catch (const std::exception& e) {
        history_.resize(history_checkpoint);
        emit(EventType::Error, "tool", e.what());
        throw;
      }
      emit(EventType::ToolCompleted, call.name, result);
      history_.push_back({Role::Tool, result, call.name, {}, call.id});
    }
  }
  const std::string error = "I stopped because the tool-step limit was reached.";
  history_.push_back({Role::Assistant, error, {}, {}, {}});
  emit(EventType::Error, "step_limit", error);
  return error;
}

}  // namespace byteturn
