#include "byteturn/mock_providers.h"
#include "byteturn/agent.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace byteturn;
using namespace std::chrono_literals;
namespace {
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
template<class F> void throws(F f) {
  bool caught = false;
  try { f(); } catch (const std::runtime_error&) { caught = true; }
  check(caught, "expected runtime error");
}
void asr_script() {
  MockAsrProvider asr({{{"h", "hello"}, "Hello."}, {{}, "Second."}});
  std::vector<std::pair<std::string, bool>> seen;
  auto sink = [&](std::string s, bool final) { seen.emplace_back(s, final); };
  asr.push({}, sink); // Empty non-final input does not consume a partial.
  for (int i = 0; i < 3; ++i) asr.push({{0}}, sink);
  asr.push({{}, 16000, 1, true}, sink);
  asr.push({{}, 16000, 1, true}, sink);
  asr.push({{}, 16000, 1, true}, sink); // Exhaustion is silent.
  check(seen == std::vector<std::pair<std::string, bool>>({
      {"h", false}, {"hello", false}, {"Hello.", true}, {"Second.", true}}), "ASR order/finality");
  asr.reset();
  asr.push({{0}}, [&](std::string s, bool final) {
    check(s == "h" && !final, "ASR reset rewinds");
    asr.reset(); // Callback is outside the provider lock.
  });
  MockAsrProvider empty(std::vector<MockAsrUtterance>{});
  empty.push({{}, 16000, 1, true}, [&](std::string, bool) { check(false, "empty script"); });
}
void llm_script() {
  MockLlmProvider llm({{{"one", "", " two"}, {}}, {{}, {{"id", "echo", "value"}}}});
  std::vector<std::string> deltas;
  auto result = llm.stream({}, [&](const auto& s) { deltas.push_back(s); return true; }, {});
  check(result.complete && result.text == "one two" &&
        deltas == std::vector<std::string>({"one", " two"}), "LLM streaming aggregation");
  result = llm.complete({});
  check(result.tool_calls.size() == 1 && result.tool_calls[0].id == "id", "structured tool reply");
  throws([&] { llm.complete({}); });
  MockLlmProvider complete({{{"a", "b"}, {}}});
  check(complete.complete({}).text == "ab", "complete aggregates deltas");
}
void llm_cancellation() {
  MockLlmProvider llm({{{"first", "second"}, {}}, {{"next"}, {}}});
  throws([&] { llm.complete({}, [] { return true; }); });
  bool cancelled = false;
  int calls = 0;
  throws([&] { llm.stream({}, [&](const auto& s) {
    ++calls; check(s == "first", "pre-cancel must not consume script");
    cancelled = true; return true;
  }, [&] { return cancelled; }); });
  check(calls == 1 && llm.complete({}).text == "next", "mid-stream cancel consumes only selected reply");
  MockLlmProvider reject({{{"a", "b"}, {}}});
  calls = 0;
  throws([&] { reject.stream({}, [&](const auto&) { ++calls; return false; }, {}); });
  check(calls == 1, "consumer rejection stops deltas");
  MockLlmProvider late({{{"last"}, {{"id", "echo", "must not execute"}}}});
  cancelled = false;
  throws([&] { late.stream({}, [&](const auto&) { cancelled = true; return true; },
                          [&] { return cancelled; }); });
  MockLlmProvider callback_error;
  throws([&] { callback_error.stream({}, [](const auto&) -> bool {
    throw std::runtime_error("callback");
  }, {}); });
}
void tts_script() {
  MockTtsProvider tts({{{1, 2, 3, 4}, 24000, 2, false}, {{5, 6}, 24000, 2, false}});
  std::vector<int> samples;
  auto sink = [&](const AudioFrame& f) {
    check(f.channels == 2 && f.sample_rate_hz == 24000, "PCM metadata");
    samples.insert(samples.end(), f.samples.begin(), f.samples.end()); return true;
  };
  tts.synthesize("", sink);
  check(samples.empty(), "empty text has no audio");
  tts.synthesize("hello", sink);
  check(samples == std::vector<int>({1, 2, 3, 4, 5, 6}), "PCM contents/order");
  int calls = 0;
  tts.synthesize("stop", [&](const auto&) { ++calls; return false; });
  check(calls == 1, "TTS consumer backpressure");
  bool invalid = false;
  try { MockTtsProvider bad({{{1}, 24000, 2, false}}); }
  catch (const std::invalid_argument&) { invalid = true; }
  check(invalid, "partial interleaved sample rejected");
}
void tts_concurrent_cancel() {
  MockTtsProvider tts({{{1}}, {{2}}});
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto resume = release.get_future();
  int calls = 0;
  tts.begin_utterance();
  auto work = std::async(std::launch::async, [&] {
    tts.synthesize_chunk("hello", [&](const auto&) {
      ++calls; entered.set_value();
      check(resume.wait_for(5s) == std::future_status::ready, "release timeout");
      return true;
    });
  });
  const bool started = ready.wait_for(5s) == std::future_status::ready;
  tts.cancel(); release.set_value(); work.get();
  check(started && calls == 1, "concurrent cancellation stops later frames");
  tts.synthesize_chunk("still cancelled", [&](const auto&) { ++calls; return true; });
  check(calls == 1, "chunk does not clear cancellation");
  tts.begin_utterance();
  tts.synthesize_chunk("fresh", [&](const auto&) { ++calls; return true; });
  check(calls == 3, "new utterance can synthesize");
}
void agent_tool_loop() {
  MockLlmProvider llm({{{}, {{"call-1", "echo", "hello"}}}, {{"Done."}, {}}});
  ToolRegistry tools;
  EventBus bus;
  std::vector<Event> events;
  bus.subscribe([&](const Event& e) { events.push_back(e); });
  int calls = 0;
  tools.add("echo", [&](const std::string& args) { ++calls; return args; });
  Agent agent(llm, tools, 8, &bus);
  check(agent.run("test", "mock-tools", "turn-1") == "Done." && calls == 1, "real agent tool loop");
  const auto& h = agent.history();
  check(h.size() == 5 && h[3].role == Role::Tool && h[3].tool_call_id == "call-1" &&
        h[3].content == "hello", "tool identity preserved in history");
  for (const auto& e : events)
    check(e.session_id == "mock-tools" && e.turn_id == "turn-1", "agent event identity");
  const auto count = [&](EventType type) {
    return std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; });
  };
  check(count(EventType::ModelStarted) == 2 && count(EventType::ModelCompleted) == 2 &&
        count(EventType::ToolStarted) == 1 && count(EventType::ToolCompleted) == 1 &&
        count(EventType::TurnCompleted) == 1 && count(EventType::TurnFailed) == 0,
        "model/tool/terminal event counts");
}
}  // namespace

int main() {
  try {
    asr_script(); llm_script(); llm_cancellation(); tts_script();
    tts_concurrent_cancel(); agent_tool_loop();
    std::cout << "6 mock provider cases passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
