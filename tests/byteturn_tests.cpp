#include "byteturn/agent.h"

#include <cstdlib>
#include <iostream>

namespace {
using namespace byteturn;

class ToolThenAnswer final : public LlmProvider {
 public:
  LlmTurn complete(const std::vector<Message>& history) override {
    if (history.back().role == Role::Tool)
      return {"result=" + history.back().content, {}, true};
    return {{}, {{"call-1", "double", "21"}}, true};
  }
};

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main() {
  ToolRegistry tools;
  require(tools.add("double", [](const std::string& value) {
    return std::to_string(std::stoi(value) * 2);
  }), "register tool");
  ToolThenAnswer llm;
  Agent agent(llm, tools);
  require(agent.run("calculate") == "result=42", "agent tool loop");
  require(agent.history().size() == 4, "history contains system/user/tool/answer");
  std::cout << "all tests passed\n";
}

