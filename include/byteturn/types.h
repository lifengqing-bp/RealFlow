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

struct Message {
  Role role;
  std::string content;
  std::string name;
};

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;
};

struct LlmTurn {
  std::string text;
  std::vector<ToolCall> tool_calls;
  bool complete = true;
};

}  // namespace byteturn

