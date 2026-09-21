#pragma once

#include "byteturn/providers.h"
#include "byteturn/event.h"
#include "byteturn/tool.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace byteturn {

class Agent {
 public:
  Agent(LlmProvider& llm, ToolRegistry& tools, std::size_t max_steps = 8,
        EventBus* events = nullptr);
  std::string run(std::string user_text);
  std::string run(std::string user_text, std::string session_id,
                  std::string turn_id,
                  std::function<bool()> cancelled = {});
  std::string run_streaming(std::string user_text, std::string session_id,
                            std::string turn_id,
                            LlmProvider::TextDeltaSink on_text_delta,
                            std::function<bool()> cancelled = {});
  const std::vector<Message>& history() const { return history_; }

 private:
  LlmProvider& llm_;
  ToolRegistry& tools_;
  std::size_t max_steps_;
  std::vector<Message> history_;
  EventBus* events_;
};

}  // namespace byteturn
