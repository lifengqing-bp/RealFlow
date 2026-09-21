#include "byteturn/conversation.h"

#include <iostream>
#include <string>

namespace {
using namespace byteturn;

class TextAsr final : public AsrProvider {
 public:
  void reset() override {}
  void push(const AudioFrame&, const std::function<void(std::string, bool)>&) override {}
};

class DemoLlm final : public LlmProvider {
 public:
  LlmTurn complete(const std::vector<Message>& history) override {
    const auto& last = history.back();
    if (last.role == Role::Tool) return {"The tool returned: " + last.content, {}, true};
    if (last.content.rfind("echo ", 0) == 0) {
      return {{}, {{"1", "echo", last.content.substr(5)}}, true};
    }
    return {"You said: " + last.content, {}, true};
  }
};

class TextTts final : public TtsProvider {
 public:
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>&) override {
    std::cout << "assistant> " << text << '\n';
  }
  void cancel() override {}
};
}  // namespace

int main() {
  TextAsr asr;
  DemoLlm llm;
  TextTts tts;
  byteturn::ToolRegistry tools;
  tools.add("echo", [](const std::string& args) { return args; });
  byteturn::Agent agent(llm, tools);

  std::cout << "ByteTurn text demo (type 'quit' to stop)\n";
  for (std::string line; std::cout << "you> " && std::getline(std::cin, line);) {
    if (line == "quit") break;
    std::cout << "assistant> " << agent.run(line) << '\n';
  }
}

