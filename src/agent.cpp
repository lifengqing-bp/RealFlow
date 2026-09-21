#include "byteturn/agent.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

bool ToolRegistry::add(std::string name, ToolHandler handler) {
  return tools_.emplace(std::move(name), std::move(handler)).second;
}

std::string ToolRegistry::invoke(const std::string& name,
                                 const std::string& arguments) const {
  const auto it = tools_.find(name);
  if (it == tools_.end()) return "error: unknown tool: " + name;
  try {
    return it->second(arguments);
  } catch (const std::exception& e) {
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
  const auto emit = [&](EventType type, std::string name = {},
                        std::string data = {}) {
    if (events_) events_->publish(
        {type, session_id, turn_id, 0, {}, std::move(name), std::move(data)});
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
    try {
      turn = llm_.complete(history_, cancelled);
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
      const auto result = tools_.invoke(call.name, call.arguments);
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
