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

Agent::Agent(LlmProvider& llm, ToolRegistry& tools, std::size_t max_steps)
    : llm_(llm), tools_(tools), max_steps_(max_steps) {
  history_.push_back({Role::System,
                      "You are ByteTurn, a concise voice agent. Use tools when useful.",
                      {}});
}

std::string Agent::run(std::string user_text) {
  history_.push_back({Role::User, std::move(user_text), {}});
  for (std::size_t step = 0; step < max_steps_; ++step) {
    const LlmTurn turn = llm_.complete(history_);
    if (!turn.text.empty()) history_.push_back({Role::Assistant, turn.text, {}});
    if (turn.tool_calls.empty()) return turn.text;

    for (const auto& call : turn.tool_calls) {
      history_.push_back(
          {Role::Tool, tools_.invoke(call.name, call.arguments), call.name});
    }
  }
  const std::string error = "I stopped because the tool-step limit was reached.";
  history_.push_back({Role::Assistant, error, {}});
  return error;
}

}  // namespace byteturn

