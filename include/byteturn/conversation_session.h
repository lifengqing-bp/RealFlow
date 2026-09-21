#pragma once

#include "byteturn/conversation_engine.h"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace byteturn {

// Owns the lifetime of a continuous human-agent interaction. A session does
// not require a listen -> think -> speak state machine and therefore permits
// user and agent activity to overlap.
class ConversationSession {
 public:
  ConversationSession(std::string session_id,
                      std::unique_ptr<ConversationEngine> engine,
                      EventBus* events = nullptr);
  ~ConversationSession();

  ConversationSession(const ConversationSession&) = delete;
  ConversationSession& operator=(const ConversationSession&) = delete;

  void start();
  bool push_audio(AudioFrame frame);
  void handle_event(Event event);
  void stop();

  const std::string& id() const { return session_id_; }
  EventTimeline& timeline() { return timeline_; }
  const EventTimeline& timeline() const { return timeline_; }

  ConversationCapabilities capabilities() const;
  ConversationStateSnapshot state() const;

 private:
  std::string session_id_;
  EventTimeline timeline_;
  std::unique_ptr<ConversationEngine> engine_;

  mutable std::mutex mutex_;
  bool started_ = false;
  bool stopped_ = false;
};

}  // namespace byteturn
