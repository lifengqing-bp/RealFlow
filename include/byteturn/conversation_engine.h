#pragma once

#include "byteturn/event_timeline.h"
#include "byteturn/providers.h"

#include <functional>
#include <string>

namespace byteturn {

struct ConversationCapabilities {
  bool native_full_duplex = false;
  bool simultaneous_listen_speak = false;
  bool native_interruption = false;
  bool native_end_of_utterance = false;
  bool native_backchannel = false;
  bool transcript_available = true;
};

struct ConversationStateSnapshot {
  bool user_speaking = false;
  bool agent_speaking = false;
  bool reasoning = false;
  bool delegated_task_running = false;
  bool interruption_pending = false;
  bool backchannel_active = false;
};

struct ConversationEngineContext {
  std::string session_id;
  EventTimeline* timeline = nullptr;  // Legacy borrowed ingress; valid until stop.
  std::uint64_t generation = 1;
  // Prefer this weak-lifetime sink for asynchronous provider observations.
  EventTimeline::Sink emit;
};

// start/stop run on the session lifecycle lane, without a session lock.
// push_audio, handle_event and state may run concurrently while Running; these
// methods must be thread-safe and bounded. stop is called only after admitted
// calls drain, and must quiesce provider callbacks, including after failed start.
// Arbitrary provider/application callbacks should call request_stop(), never a
// blocking lifecycle wait. Do not destroy the session from its callbacks.
class ConversationEngine {
 public:
  virtual ~ConversationEngine() = default;

  virtual void start(ConversationEngineContext context) = 0;
  virtual bool push_audio(AudioFrame frame) = 0;
  virtual void handle_event(const Event& event) = 0;
  virtual void stop() = 0;

  virtual ConversationCapabilities capabilities() const = 0;
  virtual ConversationStateSnapshot state() const = 0;
};

}  // namespace byteturn
