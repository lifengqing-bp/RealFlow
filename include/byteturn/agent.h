#pragma once

#include "byteturn/providers.h"
#include "byteturn/tool.h"

#include <cstddef>
#include <string>
#include <vector>

namespace byteturn {

class Agent {
 public:
  Agent(LlmProvider& llm, ToolRegistry& tools, std::size_t max_steps = 8);
  std::string run(std::string user_text);
  const std::vector<Message>& history() const { return history_; }

 private:
  LlmProvider& llm_;
  ToolRegistry& tools_;
  std::size_t max_steps_;
  std::vector<Message> history_;
};

}  // namespace byteturn

