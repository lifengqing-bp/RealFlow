#include "byteturn/agent.h"
#include "byteturn/executor.h"
#include "byteturn/session.h"
#include "byteturn/openai_compatible.h"
#include "byteturn/observability.h"
#include "byteturn/curl_transport.h"
#include "byteturn/sentence_segmenter.h"
#include "byteturn/incremental_tts.h"
#include "byteturn/conversation.h"
#include "byteturn/conversation_session.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/full_duplex_conversation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <condition_variable>

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

class ImmediateAsr final : public AsrProvider {
 public:
  void reset() override { ++resets; }
  void push(const AudioFrame& frame,
            const std::function<void(std::string, bool)>& callback) override {
    if (frame.end_of_utterance) callback("hello", true);
  }
  std::atomic<int> resets{0};
};

class BlockingAsr final : public AsrProvider {
 public:
  void reset() override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    cv.notify_all();
  }
  void push(const AudioFrame&,
            const std::function<void(std::string, bool)>&) override {
    std::unique_lock<std::mutex> lock(mutex);
    started = true;
    cv.notify_all();
    cv.wait(lock, [&] { return released; });
  }
  void wait_started() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return started; });
  }
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool released = false;
};

class BlockingStreamLlm final : public LlmProvider {
 public:
  LlmTurn complete(const std::vector<Message>&) override { return {}; }
  LlmTurn stream(const std::vector<Message>&, const TextDeltaSink& sink,
                 const std::function<bool()>& cancelled) override {
    {
      std::lock_guard<std::mutex> lock(mutex);
      started = true;
    }
    cv.notify_all();
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return released || cancelled(); });
    if (cancelled()) throw std::runtime_error("cancelled");
    lock.unlock();
    sink("Hello from the asynchronous runtime!");
    return {"Hello from the asynchronous runtime!", {}, true};
  }
  void wait_started() {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return started; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    cv.notify_all();
  }
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false;
  bool released = false;
};

class FakeRealtimeSession final : public RealtimeSpeechSession {
 public:
  explicit FakeRealtimeSession(RealtimeSpeechProvider::EventSink sink)
      : sink(std::move(sink)) {}
  bool push_audio(const AudioFrame&) override {
    ++pushed;
    return accept_audio;
  }
  void commit_input() override { ++commits; }
  void cancel_response(const std::string& id, std::uint64_t samples,
                       int rate) override {
    cancelled_id = id;
    cancelled_samples = samples;
    cancelled_rate = rate;
    ++cancels;
  }
  void close() override { closed = true; }
  void emit(RealtimeEvent event) { sink(std::move(event)); }

  RealtimeSpeechProvider::EventSink sink;
  std::atomic<int> pushed{0};
  std::atomic<int> commits{0};
  std::atomic<int> cancels{0};
  bool accept_audio = true;
  bool closed = false;
  std::string cancelled_id;
  std::uint64_t cancelled_samples = 0;
  int cancelled_rate = 0;
};

class FakeRealtimeProvider final : public RealtimeSpeechProvider {
 public:
  std::unique_ptr<RealtimeSpeechSession> connect(
      const RealtimeSessionConfig& value, EventSink sink) override {
    config = value;
    auto owned = std::make_unique<FakeRealtimeSession>(std::move(sink));
    session = owned.get();
    return owned;
  }
  RealtimeSessionConfig config;
  FakeRealtimeSession* session = nullptr;
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

void test_executor_overload_and_deadline() {
  SessionExecutor executor(SessionExecutorConfig{1, 1, 8});
  std::mutex mutex;
  std::condition_variable cv;
  bool release = false;
  bool running_started = false;
  auto blocker = executor.submit("busy", [&](const CancellationToken&,
                                              const std::string&) {
    std::unique_lock<std::mutex> lock(mutex);
    running_started = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    return std::string("done");
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return running_started; });
  }
  auto queued = executor.submit("busy", [](const CancellationToken&,
                                            const std::string&) {
    return std::string("queued");
  });
  bool overloaded = false;
  try {
    (void)executor.submit("busy", [](const CancellationToken&,
                                      const std::string&) {
      return std::string("never");
    });
  } catch (const ExecutorOverloaded&) {
    overloaded = true;
  }
  require(overloaded, "per-session pending-turn limit enforced");
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  require(blocker.result.get() == "done" && queued.result.get() == "queued",
          "accepted work survives overload rejection");

  SessionExecutor deadline_executor(SessionExecutorConfig{1, 2, 8});
  release = false;
  running_started = false;
  auto running = deadline_executor.submit("deadline", [&](const CancellationToken&,
                                                            const std::string&) {
    std::unique_lock<std::mutex> lock(mutex);
    running_started = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    return std::string("done");
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return running_started; });
  }
  TurnOptions options;
  options.timeout = std::chrono::milliseconds(5);
  auto expired = deadline_executor.submit_turn(
      "deadline", options,
      [](const TurnContext& context) {
        if (context.stop_requested())
          throw std::runtime_error("turn deadline exceeded");
        return std::string("must not complete");
      });
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  cv.notify_all();
  running.result.get();
  bool timed_out = false;
  try {
    (void)expired.result.get();
  } catch (const std::runtime_error&) {
    timed_out = true;
  }
  require(timed_out, "deadline includes time spent waiting in session queue");
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
  Event lifecycle(EventType::SessionStarted, "s");
  lifecycle.generation = 7;
  events.publish(lifecycle);
  require(logs.str().find("\"event\":\"session_started\"") != std::string::npos &&
              logs.str().find("\"generation\":7") != std::string::npos &&
              logs.str().find("\"received_at_ns\":") != std::string::npos,
          "generic lifecycle events retain non-payload timeline metadata");
  require(std::string(event_type_name(EventType::SessionStopped)) == "session_stopped" &&
              std::string(event_type_name(EventType::SessionFailed)) == "session_failed",
          "all generic lifecycle events have stable logger names");
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

void test_deadline_reaches_tool() {
  ToolThenAnswer llm;
  ToolRegistry tools;
  std::promise<void> started_promise;
  auto started = started_promise.get_future();
  require(tools.add(
              "double",
              CancellableToolHandler{
                  [&](const std::string&, const std::function<bool()>& stop) {
                    started_promise.set_value();
                    while (!stop()) std::this_thread::yield();
                    return std::string("late");
                  }}),
          "register cancellable tool");
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession session("tool-deadline", agent, executor);
  TurnOptions options;
  options.timeout = std::chrono::milliseconds(10);
  auto turn = session.submit("calculate", options);
  started.wait();
  bool timed_out = false;
  try {
    (void)turn.result.get();
  } catch (const std::runtime_error&) {
    timed_out = true;
  }
  require(timed_out, "turn deadline reaches cancellable tool");
  require(agent.history().size() == 1, "timed-out tool turn rolls back history");
}

void test_conversation_is_asynchronous() {
  ImmediateAsr asr;
  BlockingStreamLlm llm;
  RecordingTts tts;
  ToolRegistry tools;
  EventBus events;
  Agent agent(llm, tools, 8, &events);
  SessionExecutor executor(1);
  AsyncSession session("voice", agent, executor, &events);
  std::promise<void> completed_promise;
  auto completed = completed_promise.get_future();
  std::atomic<bool> completion_seen{false};
  events.subscribe([&](const Event& event) {
    if (event.type == EventType::ConversationTurnCompleted &&
        !completion_seen.exchange(true))
      completed_promise.set_value();
  });
  std::vector<AudioFrame> output;
  std::mutex output_mutex;
  Conversation conversation(
      asr, session, tts, [](const std::string&, bool) {},
      [&](const AudioFrame& frame) {
        std::lock_guard<std::mutex> lock(output_mutex);
        output.push_back(frame);
      },
      &events, "voice", ConversationConfig{4, std::chrono::milliseconds(1000), 2});

  AudioFrame frame;
  frame.samples = {1, 2, 3};
  frame.end_of_utterance = true;
  require(conversation.push_audio(frame), "audio accepted without waiting for provider");
  llm.wait_started();
  require(conversation.state() == ConversationState::Thinking,
          "LLM runs asynchronously after ASR final");
  llm.release();
  require(completed.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
          "asynchronous voice turn completes");
  std::lock_guard<std::mutex> lock(output_mutex);
  require(!output.empty(), "incremental TTS produced audio");
}

void test_full_duplex_playback_aware_barge_in() {
  FakeRealtimeProvider provider;
  EventBus events;
  MetricsRegistry metrics;
  RuntimeObserver observer(events, metrics);
  std::vector<DuplexAudio> output;
  std::vector<EventType> seen;
  events.subscribe([&](const Event& event) { seen.push_back(event.type); });

  FullDuplexConversation conversation(
      provider, "duplex", [](const std::string&, bool) {},
      [&](const DuplexAudio& audio) {
        output.push_back(audio);
        return true;
      },
      &events);
  require(provider.config.server_vad, "server VAD propagated to provider");

  AudioFrame input;
  input.samples = {9, 8, 7};
  require(conversation.push_audio(input),
          "microphone input accepted while output session remains open");
  for (int i = 0; i != 100 && provider.session->pushed.load() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(provider.session->pushed == 1, "input audio streamed asynchronously");

  provider.session->emit({RealtimeEventType::InputSpeechStarted, {}, {}, {}, {}});
  provider.session->emit({RealtimeEventType::InputSpeechEnded, {}, {}, {}, {}});
  provider.session->emit(
      {RealtimeEventType::ResponseStarted, "response-1", {}, {}, {}});
  RealtimeEvent audio;
  audio.type = RealtimeEventType::AudioDelta;
  audio.response_id = "response-1";
  audio.audio = {{1, 2, 3, 4}, 24000, 1, false};
  provider.session->emit(std::move(audio));
  require(output.size() == 1 && output[0].start_sample == 0,
          "native output audio carries a monotonic response offset");

  conversation.playback_started("response-1");
  conversation.acknowledge_playback("response-1", 2);
  provider.session->emit({RealtimeEventType::InputSpeechStarted, {}, {}, {}, {}});
  require(provider.session->cancels == 1 &&
              provider.session->cancelled_id == "response-1" &&
              provider.session->cancelled_samples == 2 &&
              provider.session->cancelled_rate == 24000,
          "barge-in truncates provider context at audible playback position");

  RealtimeEvent stale_audio;
  stale_audio.type = RealtimeEventType::AudioDelta;
  stale_audio.response_id = "response-1";
  stale_audio.audio = {{5, 6}, 24000, 1, false};
  provider.session->emit(std::move(stale_audio));
  require(output.size() == 1, "post-cancellation audio is discarded");
  require(metrics.histogram("byteturn_s2s_first_audio_latency_ms").count == 1,
          "native first-audio latency observed");
  require(metrics.histogram("byteturn_s2s_first_audible_latency_ms").count == 1,
          "physical playback boundary observed");
  require(metrics.histogram("byteturn_barge_in_stop_latency_ms").count == 1,
          "barge-in stop latency observed");
}

void test_audio_queue_overload() {
  BlockingAsr asr;
  ToolThenAnswer llm;
  RecordingTts tts;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession session("audio-overload", agent, executor);
  Conversation conversation(
      asr, session, tts, [](const std::string&, bool) {},
      [](const AudioFrame&) {}, nullptr, "audio-overload",
      ConversationConfig{1, std::chrono::milliseconds(1000), 1});
  AudioFrame frame;
  frame.samples = {1};
  require(conversation.push_audio(frame), "first audio frame accepted");
  asr.wait_started();
  require(conversation.push_audio(frame), "one audio frame may wait in queue");
  require(!conversation.push_audio(frame), "audio overload rejects newest frame");
}


class OverlapTestEngine final : public ConversationEngine {
 public:
  void start(ConversationEngineContext context) override { context_ = context; }
  bool push_audio(AudioFrame) override { return true; }
  void handle_event(const Event& event) override {
    if (event.type == EventType::InputSpeechStarted) state_.user_speaking = true;
    if (event.type == EventType::InputSpeechEnded) state_.user_speaking = false;
    if (event.type == EventType::SpeechStarted) state_.agent_speaking = true;
    if (event.type == EventType::SpeechCompleted) state_.agent_speaking = false;
  }
  void stop() override {}
  ConversationCapabilities capabilities() const override {
    ConversationCapabilities caps;
    caps.native_full_duplex = true;
    caps.simultaneous_listen_speak = true;
    return caps;
  }
  ConversationStateSnapshot state() const override { return state_; }

 private:
  ConversationEngineContext context_;
  ConversationStateSnapshot state_;
};

void test_conversation_session_allows_overlap() {
  auto engine = std::make_unique<OverlapTestEngine>();
  ConversationSession session("continuous", std::move(engine));
  session.start();

  session.handle_event({EventType::SpeechStarted, "continuous"});
  session.handle_event({EventType::InputSpeechStarted, "continuous"});

  const auto state = session.state();
  require(state.agent_speaking && state.user_speaking,
          "user and agent activity may overlap in one session");
  require(session.timeline().size() == 3,
          "session-start and overlapping semantic events are retained");
  require(session.timeline().snapshot().front().type == EventType::SessionStarted,
          "generic session lifecycle is independent of provider lifecycle");
  require(session.capabilities().simultaneous_listen_speak,
          "engine advertises simultaneous listen/speak capability");
}

// Exercise the real pipeline adapter, not only a mock state projection.
// Model/tool event unification and playback policy remain separate milestones.
void test_pipeline_engine_timeline_integration() {
  ImmediateAsr asr;
  BlockingStreamLlm llm;
  RecordingTts tts;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession agent_session("pipeline-integration", agent, executor);
  EventBus events;
  MetricsRegistry metrics;
  RuntimeObserver observer(events, metrics);
  std::promise<void> completed_promise;
  auto completed = completed_promise.get_future();
  std::atomic<bool> completion_seen{false};
  std::atomic<unsigned> audio_frames{0};
  std::vector<Event> delivered;
  const auto subscription = events.subscribe([&](const Event& event) {
    delivered.push_back(event);  // One timeline dispatcher; read after flush.
    if (event.type == EventType::ConversationTurnCompleted &&
        !completion_seen.exchange(true))
      completed_promise.set_value();
  });
  auto engine = std::make_unique<PipelineConversationEngine>(
      asr, agent_session, tts, [](const std::string&, bool) {},
      [&](const AudioFrame&) { ++audio_frames; });
  ConversationSession session("pipeline-integration", std::move(engine), &events);
  session.start();
  require(session.push_audio({{1, 2, 3}, 16000, 1, true}),
          "session forwards accepted audio to the real pipeline");
  llm.release();
  require(completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "real engine delivers a completed speech response through timeline");
  session.stop();
  require(session.timeline().flush(), "pipeline observer notifications drained");
  const auto history = session.timeline().snapshot();
  require(history.size() == delivered.size() && !history.empty(),
          "real pipeline journal and observer counts match");
  for (std::size_t i = 0; i < history.size(); ++i) {
    require(history[i].sequence == i + 1 &&
                delivered[i].sequence == history[i].sequence &&
                delivered[i].timestamp == history[i].timestamp &&
                delivered[i].received_at == history[i].received_at &&
                delivered[i].trace_id == history[i].trace_id &&
                delivered[i].generation == history[i].generation &&
                delivered[i].type == history[i].type,
            "real adapter uses canonical ordered metadata without bus rewrites");
  }
  require(history.front().type == EventType::SessionStarted &&
              history.back().type == EventType::SessionStopped,
          "real pipeline is enclosed by generic lifecycle events");
  require(audio_frames > 0 &&
              metrics.histogram("byteturn_asr_final_latency_ms").count == 1 &&
              metrics.histogram("byteturn_s2s_first_audio_latency_ms").count == 1 &&
              metrics.histogram("byteturn_tts_total_duration_ms").count == 1,
          "existing speech output and wired latency metrics survive async timeline");
  require(session.timeline().stats().dropped_notifications == 0,
          "integration fixture fits the notification budget");
  events.unsubscribe(subscription);

  auto mismatched = std::make_unique<PipelineConversationEngine>(
      asr, agent_session, tts, [](const std::string&, bool) {},
      [](const AudioFrame&) {});
  ConversationSession wrong_session("foreign", std::move(mismatched));
  bool rejected = false;
  try { wrong_session.start(); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected && wrong_session.lifecycle() == SessionLifecycle::Failed,
          "mismatched pipeline identity fails and cleans up at startup");
}

int main() {
  test_pipeline_engine_timeline_integration();
  test_conversation_session_allows_overlap();
  test_agent_and_events();
  test_session_executor();
  test_executor_overload_and_deadline();
  test_openai_compatible_adapter();
  test_openai_sse_stream();
  test_sentence_segmenter();
  test_incremental_tts();
  test_observability();
  test_curl_policy();
  test_cancellation_reaches_provider();
  test_deadline_reaches_tool();
  test_conversation_is_asynchronous();
  test_full_duplex_playback_aware_barge_in();
  test_audio_queue_overload();
  std::cout << "all tests passed\n";
}
