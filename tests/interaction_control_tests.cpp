#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/runtime_manager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace byteturn;
using namespace std::chrono_literals;
namespace {
void check(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << std::endl; std::abort(); }
}
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; cv.notify_all();
    check(cv.wait_for(lock, 5s, [&] { return released; }), "gate release timeout");
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return entered; }), "gate entry timeout");
  }
  void open() { std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all(); }
};
struct Outputs {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<int> samples;
  void push(const AudioFrame& frame) {
    std::lock_guard<std::mutex> lock(mutex);
    samples.push_back(frame.samples.front()); cv.notify_all();
  }
  void wait(std::size_t size) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return samples.size() >= size; }), "audio timeout");
  }
};
class Input final : public AsrProvider {
 public:
  Gate partial;
  std::atomic<int> resets{0};
  std::mutex mutex;
  std::condition_variable cv;
  int processed = 0;
  void reset() override { ++resets; partial.open(); }
  void push(const AudioFrame& frame,
            const std::function<void(std::string, bool)>& sink) override {
    const int marker = frame.samples.empty() ? 0 : frame.samples.front();
    if (marker == 1) sink("Find flights to Tokyo.", true);
    if (marker == 2) { partial.block(); sink("Actually, Osa", false); }
    if (marker == 3) sink("Actually, Osaka. Do not book anything.", true);
    { std::lock_guard<std::mutex> lock(mutex); ++processed; cv.notify_all(); }
  }
  void wait(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    check(cv.wait_for(lock, 5s, [&] { return processed >= count; }), "ASR timeout");
  }
};
class Work final : public LlmProvider {
 public:
  Gate first;
  std::atomic<bool> late_token_accepted{false};
  LlmTurn complete(const std::vector<Message>&) override { return {}; }
  LlmTurn stream(const std::vector<Message>& history, const TextDeltaSink& sink,
                 const std::function<bool()>&) override {
    if (history.back().content.find("Tokyo") != std::string::npos) {
      sink("Searching for flights to Tokyo!");
      first.block();
      // Deliberately ignore cancellation, as an adversarial provider fixture.
      late_token_accepted = sink("This Tokyo result is now obsolete!");
      return {"obsolete", {{"late", "book", "Tokyo"}}, true};
    }
    sink("Here are the flights to Osaka!");
    return {"Here are the flights to Osaka!", {}, true};
  }
};
class Speech final : public TtsProvider {
 public:
  std::atomic<int> begins{0}, cancels{0};
  void begin_utterance() override { ++begins; }
  void cancel() override { ++cancels; }
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>& sink) override {
    const auto value = text.find("Osaka") != std::string::npos ? 2 : 1;
    sink({{static_cast<std::int16_t>(value)}, 16000, 1, false});
  }
};
struct Pipeline {
  Input asr;
  Work llm;
  Speech tts;
  ToolRegistry tools;
  std::atomic<int> bookings{0};
  Agent agent{llm, tools};
  SessionExecutor executor{1};
  AsyncSession legacy{"human", agent, executor};
  Outputs output;
  std::mutex transcript_mutex;
  std::vector<std::string> transcripts;
  Pipeline() {
    tools.add("book", [&](const std::string&) { ++bookings; return "booked"; });
  }
  std::unique_ptr<ConversationEngine> engine() {
    return std::make_unique<PipelineConversationEngine>(
        asr, legacy, tts,
        [&](const std::string& text, bool) {
          std::lock_guard<std::mutex> lock(transcript_mutex); transcripts.push_back(text);
        },
        [&](const AudioFrame& frame) { output.push(frame); });
  }
};
AudioFrame frame(int marker, bool final = false) {
  return {{static_cast<std::int16_t>(marker)}, 16000, 1, final};
}
void continuous_audio_is_not_a_command() {
  Pipeline p;
  ConversationSession session("human", p.engine()); session.start();
  check(session.capabilities().response_cancellation, "pipeline advertises cancellation");
  session.push_audio(frame(1, true)); p.llm.first.wait(); p.output.wait(1);
  for (int i = 0; i < 8; ++i) check(session.push_audio(frame(0)), "silence accepted");
  p.asr.wait(9);
  check(p.tts.cancels == 0, "PCM/silence must not cancel a response");
  check(session.state().agent_speaking, "input must not overwrite output state");
  check(bool(session.observe({EventType::TurnCancelled})), "observation recorded");
  session.handle_event({EventType::InputSpeechStarted});
  check(p.tts.cancels == 0, "observations and legacy events do not execute cancellation");
  check(session.cancel_response(), "explicit cancellation requested");
  check(p.asr.resets == 0, "cancellation leaves ASR alone");
  p.llm.first.open(); session.stop();
  check(!session.cancel_response(), "stopped session rejects cancellation");
}
void correction_survives_response_cancellation() {
  auto p = std::make_shared<Pipeline>();
  RuntimeManager runtime;
  auto a = runtime.add("human", [p](const SessionHandle&) {
    SessionResources r; r.dependencies = p; r.engine = p->engine(); return r;
  });
  check(a.started.wait_for(5s) == std::future_status::ready &&
        a.started.get() == SessionOutcome::Started, "managed startup");
  runtime.push_audio(a.handle, frame(1, true)); p->llm.first.wait(); p->output.wait(1);
  runtime.push_audio(a.handle, frame(2)); p->asr.partial.wait();
  // This final is already queued when cancel_response runs. Clearing/resetting
  // ASR here would lose either this frame or the in-flight partial callback.
  runtime.push_audio(a.handle, frame(3, true));
  auto stale = a.handle; ++stale.generation;
  check(!runtime.cancel_response(stale), "stale handle cannot cancel replacement work");
  check(p->tts.cancels == 0, "stale command has no side effect");
  check(runtime.cancel_response(a.handle), "cancel Tokyo response through manager");
  check(runtime.find(a.handle)->phase == SessionPhase::Running, "cancel does not retire session");
  p->asr.partial.open(); p->asr.wait(3);
  check(p->asr.resets == 0, "correction input retained without resetting ASR");
  check(p->tts.begins == 1, "queued correction does not open a concurrent TTS utterance");
  p->llm.first.open(); p->output.wait(2);
  runtime.remove(a.handle);
  check(a.retired.wait_for(5s) == std::future_status::ready, "managed retirement");
  check(!p->llm.late_token_accepted, "late cancelled tokens never reach speech consumer");
  check(p->bookings == 0, "cancelled model cannot dispatch a late tool call");
  check(p->output.samples == std::vector<int>({1, 2}), "only Osaka follows cancellation");
  check(p->transcripts == std::vector<std::string>({
        "Find flights to Tokyo.", "Actually, Osa", "Actually, Osaka. Do not book anything."}),
        "in-flight and queued correction transcripts both survive");
  check(!runtime.cancel_response(a.handle), "retired handle rejects command");
}
void cancelled_synthesis_cannot_deliver_late_audio() {
  class Answer final : public LlmProvider {
   public:
    LlmTurn complete(const std::vector<Message>&) override {
      return {"This answer is long enough to speak!", {}, true};
    }
  } llm;
  class LateSpeech final : public TtsProvider {
   public:
    Gate after_first;
    std::atomic<bool> late_accepted{true};
    void cancel() override {}  // Deliberately ignores the cancellation request.
    void synthesize(const std::string&,
                    const std::function<bool(const AudioFrame&)>& sink) override {
      sink({{1}, 16000, 1, false});
      after_first.block();
      late_accepted = sink({{99}, 16000, 1, false});
    }
  } tts;
  Input asr;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession legacy("late-audio", agent, executor);
  Outputs output;
  ConversationSession session("late-audio",
      std::make_unique<PipelineConversationEngine>(asr, legacy, tts,
          [](const std::string&, bool) {},
          [&](const AudioFrame& f) { output.push(f); }));
  session.start(); session.push_audio(frame(1, true));
  output.wait(1); tts.after_first.wait();
  check(session.cancel_response(), "cancel does not wait for blocked synthesis");
  tts.after_first.open(); session.stop();
  check(!tts.late_accepted && output.samples == std::vector<int>({1}),
        "cancelled generation rejects late audio before reaching the player");
  for (const auto& event : session.timeline().snapshot())
    check(event.type != EventType::ConversationTurnCompleted,
          "cancelled synthesis must not report successful conversation completion");
}
class ControlEngine final : public ConversationEngine {
 public:
  std::function<bool()> cancel;
  void start(ConversationEngineContext) override {}
  bool push_audio(AudioFrame) override { return true; }
  void handle_event(const Event&) override {}
  bool cancel_response() override { return cancel ? cancel() : false; }
  void stop() override {}
  ConversationCapabilities capabilities() const override { return {}; }
  ConversationStateSnapshot state() const override { return {}; }
};
void unsupported_and_lifecycle_admission() {
  auto engine = std::make_unique<ControlEngine>();
  ConversationSession session("s", std::move(engine));
  check(!session.cancel_response(), "unstarted session rejects command");
  session.start(); check(!session.cancel_response(), "unsupported engine rejects command");
  session.stop(); check(!session.cancel_response(), "stopped session rejects command");
}
void cancellation_lease_and_reentry() {
  Gate gate;
  auto engine = std::make_unique<ControlEngine>();
  ConversationSession* owner = nullptr;
  engine->cancel = [&] { owner->stop(); gate.block(); return true; };
  ConversationSession session("s", std::move(engine)); owner = &session;
  session.start();
  auto call = std::async(std::launch::async, [&] { return session.cancel_response(); });
  gate.wait();
  check(!session.wait_stopped(1ms), "shutdown waits for admitted cancellation call");
  gate.open(); check(call.get(), "callback-safe stop did not self-deadlock");
  check(session.wait_stopped(5s), "operation lease released");
}
void manager_contains_cancellation_exception() {
  RuntimeManager runtime;
  auto a = runtime.add("fail", [](const SessionHandle&) {
    auto e = std::make_unique<ControlEngine>();
    e->cancel = []() -> bool { throw std::runtime_error("cancel failed"); };
    SessionResources r; r.engine = std::move(e); return r;
  });
  check(a.started.wait_for(5s) == std::future_status::ready, "startup timeout");
  check(!runtime.cancel_response(a.handle), "engine exception contained");
  check(a.retired.wait_for(5s) == std::future_status::ready &&
        a.retired.get() == SessionOutcome::EngineFailure, "failure retired safely");
}
}  // namespace
int main() {
  continuous_audio_is_not_a_command();
  correction_survives_response_cancellation();
  cancelled_synthesis_cannot_deliver_late_audio();
  unsupported_and_lifecycle_admission();
  cancellation_lease_and_reentry();
  manager_contains_cancellation_exception();
  std::cout << "6 interaction control tests passed\n";
}
