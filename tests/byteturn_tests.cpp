#include "byteturn/agent.h"
#include "byteturn/executor.h"
#include "byteturn/session.h"
#include "byteturn/openai_compatible.h"
#include "byteturn/observability.h"
#include "byteturn/curl_transport.h"
#include "byteturn/sentence_segmenter.h"
#include "byteturn/incremental_tts.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
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
    for (const auto& chunk : stream_chunks)
      if (request.body_sink && !request.body_sink(chunk)) break;
    return response;
  }
  HttpRequest last_request;
  HttpResponse response;
  std::vector<std::string> stream_chunks;
};

class RecordingTts final : public TtsProvider {
 public:
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>& on_audio) override {
    synthesize_chunk(text, on_audio);
  }
  void begin_utterance() override { ++begins; }
  void synthesize_chunk(
      const std::string& text,
      const std::function<bool(const AudioFrame&)>& on_audio) override {
    chunks.push_back(text);
    on_audio({{1, 2, 3}, 16000, 1, false});
  }
  void end_utterance() override { ++ends; }
  void cancel() override { ++cancels; }
  std::vector<std::string> chunks;
  int begins = 0;
  int ends = 0;
  int cancels = 0;
};

class CancellableLlm final : public LlmProvider {
 public:
  LlmTurn complete(const std::vector<Message>&) override { return {}; }
  LlmTurn complete(const std::vector<Message>&,
                   const std::function<bool()>& cancelled) override {
    started.set_value();
    while (!cancelled()) std::this_thread::yield();
    throw std::runtime_error("network cancelled");
  }
  std::promise<void> started;
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
      R"({"choices":[{"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Sydney\"}"}}]}}]})",
      {}, 0.0, 0.0, 0.0, 0.0, 0.0};
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

void test_openai_sse_stream() {
  FakeHttp http;
  http.response.status = 200;
  http.stream_chunks = {
      "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\" world. \"}}]}\n",
      "\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_\",\"function\":{\"name\":\"wea\",\"arguments\":\"{\\\"city\\\":\"}}]}}]}\n\n",
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"1\",\"function\":{\"name\":\"ther\",\"arguments\":\"\\\"Sydney\\\"}\"}}]}}]}\n\n"
      "data: [DONE]\n\n"};
  OpenAiCompatibleConfig config;
  config.base_url = "https://example.test/v1";
  config.model = "stream-model";
  OpenAiCompatibleLlm llm(config, http);
  std::string observed;
  const auto turn = llm.stream(
      {{Role::User, "hello", {}, {}, {}}},
      [&](const std::string& delta) {
        observed += delta;
        return true;
      },
      [] { return false; });
  require(observed == "Hello world. ", "SSE text deltas delivered in order");
  require(turn.text == observed && turn.complete, "SSE completion assembled");
  require(turn.tool_calls.size() == 1 && turn.tool_calls[0].id == "call_1" &&
              turn.tool_calls[0].name == "weather" &&
              turn.tool_calls[0].arguments == R"({"city":"Sydney"})",
          "streamed tool call fragments assembled by index");
  require(http.last_request.body.find("\"stream\":true") != std::string::npos,
          "streaming requested");
}

void test_sentence_segmenter() {
  SentenceSegmenter segmenter({4, 12, 24});
  require(segmenter.push("Hello world.").empty(),
          "period waits for following whitespace");
  auto english = segmenter.push(" Next sentence!");
  require(english.size() == 2 && english[0] == "Hello world." &&
              english[1] == "Next sentence!",
          "English boundaries across token chunks");
  segmenter.reset();
  auto chinese = segmenter.push("这是第一句话。这里是第二句话！");
  require(chinese.size() == 2, "Chinese punctuation segmented");
}

void test_incremental_tts() {
  RecordingTts tts;
  std::vector<std::size_t> frames;
  IncrementalTtsPipeline pipeline(
      tts, [&](const AudioFrame& frame) {
        frames.push_back(frame.samples.size());
        return true;
      });
  require(pipeline.push("one."), "first TTS chunk queued");
  require(pipeline.push("two."), "second TTS chunk queued");
  pipeline.finish();
  require(tts.begins == 1 && tts.ends == 1 && tts.cancels == 0,
          "incremental utterance lifecycle");
  require(tts.chunks == std::vector<std::string>({"one.", "two."}),
          "TTS chunks preserve order");
  require(frames.size() == 2, "audio streamed from every TTS chunk");
}

void test_observability() {
  EventBus events;
  MetricsRegistry metrics;
  RuntimeObserver observer(events, metrics);
  std::ostringstream logs;
  JsonEventLogger logger(events, logs);
  const auto start = std::chrono::steady_clock::now();
  events.publish({EventType::TurnStarted, "s", "t", 0, start, {}, "secret"});
  events.publish({EventType::AsrEndOfUtterance, "s", "t", 0, start, {}, {}});
  events.publish({EventType::TranscriptFinal, "s", "t", 0,
                  start + std::chrono::milliseconds(20), {}, {}});
  events.publish({EventType::ModelStarted, "s", "t", 0, start, "0", {}});
  events.publish({EventType::FirstToken, "s", "t", 0,
                  start + std::chrono::milliseconds(7), "0", {}});
  events.publish({EventType::ModelCompleted, "s", "t", 0,
                  start + std::chrono::milliseconds(30), "0", {}});
  events.publish({EventType::SpeechStarted, "s", "t", 0,
                  start + std::chrono::milliseconds(35), {}, {}});
  events.publish({EventType::FirstAudio, "s", "t", 0,
                  start + std::chrono::milliseconds(50), {}, {}});
  events.publish({EventType::SpeechCompleted, "s", "t", 0,
                  start + std::chrono::milliseconds(90), {}, {}});
  events.publish({EventType::ConversationTurnCompleted, "s", "t", 0,
                  start + std::chrono::milliseconds(90), {}, {}});
  events.publish({EventType::TurnCompleted, "s", "t", 0,
                  start + std::chrono::milliseconds(12), {}, "private reply"});
  require(metrics.counter("byteturn_events_turn_started_total") == 1,
          "event counter");
  const auto histogram = metrics.histogram("byteturn_turn_duration_ms");
  require(histogram.count == 1 && histogram.sum >= 12.0,
          "turn latency histogram");
  require(metrics.prometheus_text().find("byteturn_turn_duration_ms_count 1") !=
              std::string::npos,
          "Prometheus rendering");
  require(metrics.histogram("byteturn_time_to_first_token_ms").count == 1,
          "time-to-first-token histogram");
  require(metrics.histogram("byteturn_asr_final_latency_ms").count == 1,
          "ASR final latency histogram");
  require(metrics.histogram("byteturn_model_duration_ms").count == 1,
          "LLM total latency histogram");
  require(metrics.histogram("byteturn_tts_first_audio_latency_ms").count == 1,
          "TTS first-audio histogram");
  require(metrics.histogram("byteturn_tts_total_duration_ms").count == 1,
          "TTS total histogram");
  require(metrics.histogram("byteturn_s2s_first_audio_latency_ms").count == 1,
          "speech-to-speech first-audio histogram");
  require(metrics.histogram("byteturn_conversation_turn_duration_ms").count == 1,
          "end-to-end turn histogram");
  require(logs.str().find("private reply") == std::string::npos,
          "event payload redacted by default");
  require(logs.str().find("\"session_id\":\"s\"") != std::string::npos,
          "structured correlation fields");
}

void test_curl_policy() {
  require(CurlHttpTransport::retryable_http_status(429), "429 is retryable");
  require(CurlHttpTransport::retryable_http_status(503), "503 is retryable");
  require(!CurlHttpTransport::retryable_http_status(400), "400 is not retryable");
  require(CurlHttpTransport::parse_retry_after("3") ==
              std::chrono::milliseconds(3000),
          "Retry-After seconds");
}

void test_cancellation_reaches_provider() {
  CancellableLlm llm;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession session("cancel-test", agent, executor);
  auto started = llm.started.get_future();
  auto turn = session.submit("hello");
  started.wait();
  session.cancel();
  bool cancelled = false;
  try {
    (void)turn.result.get();
  } catch (const std::runtime_error&) {
    cancelled = true;
  }
  require(cancelled, "session cancellation reaches provider");
  require(agent.history().size() == 1, "cancelled turn rolls back history");
}

int main() {
  test_agent_and_events();
  test_session_executor();
  test_openai_compatible_adapter();
  test_openai_sse_stream();
  test_sentence_segmenter();
  test_incremental_tts();
  test_observability();
  test_curl_policy();
  test_cancellation_reaches_provider();
  std::cout << "all tests passed\n";
}
