#include "byteturn/websocket_mock_providers.h"
#include "byteturn/mock_providers.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/conversation_session.h"

#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace byteturn;
using namespace std::chrono_literals;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> std::string failure(F f) {
  try { f(); } catch (const std::exception& e) { return e.what(); }
  throw std::logic_error("expected request failure");
}
std::vector<Message> history() { return {{Role::User, "Hello 世界.", {}, {}, {}}}; }

void success(WebSocketMockConfig config) {
  WebSocketMockAsr asr(config);
  std::vector<std::pair<std::string, bool>> transcripts;
  const auto sink = [&](std::string s, bool final) { transcripts.emplace_back(s, final); };
  asr.push({{0}}, sink);
  asr.push({{0}}, sink); // Same connection: only one partial per utterance.
  asr.push({{}, 16000, 1, true}, sink);
  asr.reset();
  asr.push({{0}}, sink);
  check(transcripts == std::vector<std::pair<std::string, bool>>({
      {"Hel", false}, {"Hello.", true}, {"Hel", false}}), "ASR stream/reset state");
  WebSocketMockLlm llm(config);
  int deltas = 0;
  const auto answer = llm.stream(history(), [&](const auto&) { ++deltas; return true; }, {});
  check(answer.complete && answer.text == "You said: Hello 世界." && deltas == 2,
        "LLM history serialization, UTF-8 and streamed aggregation");
  check(llm.complete(history()).text == answer.text, "complete matches stream");
  check(!failure([&] { llm.complete(history(), [] { return true; }); }).empty(), "pre-cancelled");
  int rejected = 0;
  failure([&] { llm.stream(history(), [&](const auto&) { ++rejected; return false; }, {}); });
  check(rejected == 1 && llm.complete(history()).text == answer.text, "rejection drops old connection");
  ToolRegistry tools;
  int tool_calls = 0;
  tools.add("echo", [&](const std::string& args) { ++tool_calls; return args; });
  Agent agent(llm, tools);
  check(agent.run("echo hello") == "The tool returned: hello" && tool_calls == 1,
        "structured tool calls across WebSocket");
  WebSocketMockTts tts(config);
  int frames = 0;
  tts.synthesize("hello", [&](const AudioFrame& f) {
    ++frames;
    check(f.samples.size() == 160 && f.channels == 1 && f.sample_rate_hz == 16000,
          "TTS PCM payload and metadata");
    return true;
  });
  check(frames == 2, "streamed TTS frames");
  frames = 0;
  tts.synthesize("hello", [&](const AudioFrame&) { ++frames; return false; });
  check(frames == 1, "TTS backpressure");
  tts.cancel();
  tts.synthesize_chunk("cancelled", [&](const AudioFrame&) { ++frames; return true; });
  check(frames == 1, "TTS chunk cannot clear cancellation");
}

void cancellation(WebSocketMockConfig config, const std::string& service) {
  std::promise<void> first;
  auto ready = first.get_future();
  std::atomic<bool> stop{false};
  int callbacks = 0;
  WebSocketMockLlm llm(config);
  WebSocketMockAsr asr(config);
  WebSocketMockTts tts(config);
  tts.begin_utterance();
  auto task = std::async(std::launch::async, [&] {
    const auto entered = [&] { ++callbacks; first.set_value(); };
    if (service == "llm") {
      const auto reason = failure([&] { llm.stream(history(), [&](const auto&) {
        entered(); return true;
      }, [&] { return stop.load(); }); });
      check(reason.find("cancelled") != std::string::npos, "LLM cancelled while waiting for next delta");
    } else if (service == "tts") {
      tts.synthesize_chunk("hello", [&](const auto&) { entered(); return true; });
    } else {
      asr.push({{0}}, [&](std::string, bool) { entered(); });
    }
  });
  const bool got_first = ready.wait_for(3s) == std::future_status::ready;
  const auto begin = std::chrono::steady_clock::now();
  stop = true; tts.cancel(); asr.reset();
  check(task.wait_for(1s) == std::future_status::ready, "cancellation must wake blocked receive");
  task.get();
  check(got_first && callbacks == 1 && std::chrono::steady_clock::now() - begin < 1s,
        "one callback then bounded cancellation");
}

void limits(WebSocketMockConfig config) {
  auto small = config;
  small.max_response_messages = 1;
  WebSocketMockLlm count(small);
  check(failure([&] { count.complete(history()); }).find("message limit") != std::string::npos,
        "response message bound");
  small = config; small.max_response_bytes = 16;
  WebSocketMockLlm bytes(small);
  check(failure([&] { bytes.complete(history()); }).find("size limit") != std::string::npos,
        "aggregate response bound");
  small = config; small.max_message_bytes = 128;
  WebSocketMockLlm request(small);
  check(failure([&] { request.complete({{Role::User, std::string(1024, 'x'), {}, {}, {}}}); })
        .find("too large") != std::string::npos, "outbound history bound");
}

void asr_worker_failure(WebSocketMockConfig config) {
  // Exercise the actual audio worker: I/O errors must be observable, not terminate.
  WebSocketMockAsr asr(config);
  MockLlmProvider llm;
  MockTtsProvider tts;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession turns("failure", agent, executor);
  std::promise<void> error;
  std::promise<void> second_error;
  auto seen = error.get_future();
  auto seen_again = second_error.get_future();
  int errors = 0; // Timeline callbacks run serially and are drained below.
  ConversationSession session("failure", std::make_unique<PipelineConversationEngine>(
      asr, turns, tts, [](const auto&, bool) {}, [](const auto&) {}));
  session.timeline().subscribe([&](const Event& e) {
    if (e.type == EventType::Error && e.name == "asr") {
      if (++errors == 1) error.set_value();
      else if (errors == 2) second_error.set_value();
    }
  });
  session.start();
  check(session.push_audio({{0}}), "input admitted");
  const bool observed = seen.wait_for(3s) == std::future_status::ready;
  check(session.lifecycle() == SessionLifecycle::Running, "ASR failure does not terminate session");
  check(session.push_audio({{0}}), "input still admitted after failure");
  const bool observed_again = seen_again.wait_for(3s) == std::future_status::ready;
  session.stop();
  check(session.timeline().flush(1s) && observed && observed_again && errors == 2,
        "ASR worker processes subsequent input and drains failures");
  const auto events = session.timeline().snapshot();
  for (const auto& e : events) {
    check(e.session_id == "failure", "ASR failure identity");
    check(e.type != EventType::ConversationTurnCompleted, "no fabricated successful turn");
  }
  check(session.timeline().stats().dropped_notifications == 0, "complete failure trace");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    check(argc == 3, "usage: websocket_mock_tests PORT MODE");
    WebSocketMockConfig config;
    const int port = std::stoi(argv[1]);
    check(port > 0 && port <= 65535, "invalid port");
    config.port = static_cast<std::uint16_t>(port);
    const std::string mode = argv[2];
    if (mode == "success") { success(config); limits(config); }
    else if (mode.rfind("cancel_", 0) == 0) cancellation(config, mode.substr(7));
    else if (mode == "asr_failure") asr_worker_failure(config);
    else if (mode == "bad_pcm") {
      WebSocketMockTts tts(config);
      int callbacks = 0;
      check(failure([&] { tts.synthesize("hello", [&](const auto&) { ++callbacks; return true; }); })
            .find("invalid mock PCM") != std::string::npos, "invalid PCM classified");
      check(callbacks == 0, "invalid PCM never delivered");
    } else {
      if (mode == "stall") config.timeout = 150ms;
      WebSocketMockLlm llm(config);
      const auto reason = failure([&] { llm.complete(history()); });
      if (mode == "stall") check(reason.find("deadline") != std::string::npos, "absolute deadline");
      if (mode == "wrong_id") check(reason.find("identity mismatch") != std::string::npos, "wrong ID rejected");
      if (mode == "malformed") check(reason == "invalid mock WebSocket JSON", "malformed payload redacted");
      if (mode == "error") check(reason.find("supplier reported") != std::string::npos, "supplier error rejected");
      if (mode == "disconnect") check(reason.find("I/O") != std::string::npos, "disconnect classified");
      if (mode == "oversize") check(reason.find("limit") != std::string::npos, "message bound enforced");
    }
    std::cout << "PASS WebSocket " << mode << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
