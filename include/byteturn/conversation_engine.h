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
  EventTimeline* timeline = nullptr;
};

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
