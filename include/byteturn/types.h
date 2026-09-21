#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace byteturn {

struct AudioFrame {
  std::vector<std::int16_t> samples;
  int sample_rate_hz = 16000;
  int channels = 1;
  bool end_of_utterance = false;
};

enum class Role { System, User, Assistant, Tool };

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct Message {
  Role role;
  std::string content;
  std::string name;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
};

struct LlmTurn {
  std::string text;
  std::vector<ToolCall> tool_calls;
  bool complete = true;
};

}  // namespace byteturn
