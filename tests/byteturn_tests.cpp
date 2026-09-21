#include "byteturn/agent.h"
#include "byteturn/executor.h"
#include "byteturn/openai_compatible.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

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

class FakeHttp final : public HttpTransport {
 public:
  HttpResponse perform(const HttpRequest& request) override {
    last_request = request;
    return response;
  }
  HttpRequest last_request;
  HttpResponse response;
};

void require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

void test_agent_and_events() {
  ToolRegistry tools;
  require(tools.add("double", [](const std::string& value) {
    return std::to_string(std::stoi(value) * 2);
  }), "register tool");
  ToolThenAnswer llm;
  EventBus events;
  std::vector<EventType> seen;
  events.subscribe([&](const Event& event) { seen.push_back(event.type); });
  Agent agent(llm, tools, 8, &events);
  require(agent.run("calculate", "session-1", "turn-1") == "result=42",
          "agent tool loop");
  require(agent.history().size() == 5,
          "history contains system/user/assistant-tool/tool/answer");
  require(seen.front() == EventType::TurnStarted, "turn-start event");
  require(seen.back() == EventType::TurnCompleted, "turn-complete event");
}

void test_session_executor() {
  SessionExecutor executor(2);
  std::atomic<int> active_a{0};
  std::atomic<int> max_active_a{0};
  auto same_session_task = [&](const CancellationToken&, const std::string&) {
    const int active = ++active_a;
    int observed = max_active_a.load();
    while (active > observed &&
           !max_active_a.compare_exchange_weak(observed, active)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    --active_a;
    return std::string("ok");
  };
  auto first = executor.submit("a", same_session_task);
  auto second = executor.submit("a", same_session_task);
  require(first.result.get() == "ok" && second.result.get() == "ok",
          "session tasks complete");
  require(max_active_a == 1, "turns in one session are serialized");

  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  auto parallel_task = [&](const CancellationToken&, const std::string&) {
    const int count = ++active;
    int observed = max_active.load();
    while (count > observed && !max_active.compare_exchange_weak(observed, count)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    --active;
    return std::string("ok");
  };
  auto a = executor.submit("a", parallel_task);
  auto b = executor.submit("b", parallel_task);
  a.result.get();
  b.result.get();
  require(max_active == 2, "different sessions can run concurrently");
}

void test_openai_compatible_adapter() {
  FakeHttp http;
  http.response = {200,
      R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Sydney\"}"}}]}}]})"};
  OpenAiCompatibleConfig config;
  config.base_url = "https://example.test/v1/";
  config.api_key = "secret";
  config.model = "test-model";
  config.tools.push_back({"weather", "Get weather",
                          R"({"type":"object","properties":{"city":{"type":"string"}}})"});
  OpenAiCompatibleLlm llm(config, http);
  const auto turn = llm.complete({{Role::User, "weather?", {}, {}, {}}});
  require(http.last_request.url == "https://example.test/v1/chat/completions",
          "chat completions endpoint");
  require(http.last_request.headers.at("Authorization") == "Bearer secret",
          "bearer authentication");
  require(http.last_request.body.find("\"tools\"") != std::string::npos,
          "tool definitions encoded");
  require(turn.tool_calls.size() == 1 && turn.tool_calls[0].name == "weather",
          "tool call decoded");
  require(turn.tool_calls[0].arguments == R"({"city":"Sydney"})",
          "tool arguments decoded");
}

int main() {
  test_agent_and_events();
  test_session_executor();
  test_openai_compatible_adapter();
  std::cout << "all tests passed\n";
}
