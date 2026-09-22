#include "byteturn/mock_providers.h"
#include "byteturn/pipeline_conversation_engine.h"
#include "byteturn/conversation_session.h"
#include "byteturn/observability.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>

int main() {
  using namespace byteturn;
  using namespace std::chrono_literals;
  // Dependencies precede the session: all callbacks drain before these die.
  MockAsrProvider asr({{{"Hel"}, "Hello."}});
  MockLlmProvider llm({{{"Hello ", "from RealFlow."}, {}}});
  MockTtsProvider tts;
  ToolRegistry tools;
  Agent agent(llm, tools);
  SessionExecutor executor(1);
  AsyncSession turns("mock-demo", agent, executor);
  MetricsRegistry metrics;
  RuntimeObserver observer(metrics);
  std::mutex mutex;
  std::condition_variable cv;
  bool completed = false, failed = false;
  std::size_t samples = 0, finals = 0;
  ConversationSession session("mock-demo",
      std::make_unique<PipelineConversationEngine>(asr, turns, tts,
          [&](const std::string&, bool final) { if (final) ++finals; },
          [&](const AudioFrame& frame) { samples += frame.samples.size(); }));
  session.timeline().subscribe([&](const Event& e) {
    observer.observe(e);
    std::lock_guard<std::mutex> lock(mutex);
    if (e.type == EventType::ConversationTurnCompleted) completed = true;
    if (e.type == EventType::Error || e.type == EventType::TurnFailed) failed = true;
    cv.notify_all();
  });
  session.start();
  if (!session.push_audio({{0}, 16000, 1, false}) ||
      !session.push_audio({{}, 16000, 1, true})) return 1;
  bool ready;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ready = cv.wait_for(lock, 5s, [&] { return completed || failed; });
  }
  session.stop();
  if (!session.timeline().flush(5s)) return 1;
  const auto events = session.timeline().snapshot();
  const auto position = [&](EventType type) {
    return std::find_if(events.begin(), events.end(),
                        [&](const Event& e) { return e.type == type; }) - events.begin();
  };
  const auto end = static_cast<std::ptrdiff_t>(events.size());
  const bool ordered = position(EventType::AsrEndOfUtterance) < position(EventType::TranscriptFinal) &&
      position(EventType::TranscriptFinal) < position(EventType::ModelStarted) &&
      position(EventType::ModelStarted) < position(EventType::FirstToken) &&
      position(EventType::FirstToken) < position(EventType::ModelCompleted) &&
      position(EventType::FirstAudio) < position(EventType::ConversationTurnCompleted) &&
      position(EventType::ConversationTurnCompleted) < end;
  std::uint64_t sequence = 0;
  bool identity = true;
  for (const auto& e : events) {
    identity = identity && e.session_id == "mock-demo" && e.generation == 1 &&
        e.sequence > sequence;
    if (e.type == EventType::ConversationTurnCompleted)
      identity = identity && !e.turn_id.empty() && !e.trace_id.empty();
    sequence = e.sequence;
  }
  // These metrics are pipeline endpoint checks, not real-provider benchmarks.
  const bool ok = ready && completed && !failed && ordered && identity &&
      finals == 1 && samples == 160 &&
      metrics.histogram("byteturn_asr_final_latency_ms").count == 1 &&
      metrics.histogram("byteturn_time_to_first_token_ms").count == 1 &&
      metrics.histogram("byteturn_tts_first_audio_latency_ms").count == 1 &&
      metrics.histogram("byteturn_conversation_turn_duration_ms").count == 1 &&
      session.timeline().stats().dropped_notifications == 0;
  std::cout << "Offline mock pipeline: finals=" << finals << " samples=" << samples
            << " completed=" << completed << " verified=" << ok << '\n';
  return ok ? 0 : 1;
}
