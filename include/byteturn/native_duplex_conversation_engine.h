#pragma once

#include "byteturn/conversation_engine.h"
#include "byteturn/full_duplex_conversation.h"

namespace byteturn {

// Borrows the provider through stop/destruction. Session lifecycle admission
// protects conversation_ lifetime; FullDuplexConversation owns media state.
// Sinks must be bounded; callbacks request_stop rather than wait for shutdown.
class NativeDuplexConversationEngine final : public ConversationEngine {
 public:
  NativeDuplexConversationEngine(RealtimeSpeechProvider& provider,
      FullDuplexConversation::TranscriptSink transcript_sink,
      FullDuplexConversation::AudioSink audio_sink, FullDuplexConfig config = {});
  ~NativeDuplexConversationEngine() override;
  void start(ConversationEngineContext context) override;
  bool push_audio(AudioFrame frame) override;
  bool cancel_response() override;
  bool playback_started(const std::string& response_id) override;
  bool acknowledge_playback(const std::string& response_id,
                            std::uint64_t played_samples) override;
  void handle_event(const Event&) override;
  void stop() override;
  ConversationCapabilities capabilities() const override;
  ConversationStateSnapshot state() const override;

 private:
  RealtimeSpeechProvider& provider_;
  FullDuplexConversation::TranscriptSink transcript_sink_;
  FullDuplexConversation::AudioSink audio_sink_;
  FullDuplexConfig config_;
  EventBus bridge_bus_;
  EventBus::Subscription bridge_subscription_ = 0;
  std::unique_ptr<FullDuplexConversation> conversation_;
};

}  // namespace byteturn
