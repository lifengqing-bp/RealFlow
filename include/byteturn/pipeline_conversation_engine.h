#pragma once

#include "byteturn/conversation.h"
#include "byteturn/conversation_engine.h"

#include <memory>
#include <mutex>

namespace byteturn {

// Transitional adapter that runs the existing ASR -> Agent/LLM -> TTS
// implementation behind the provider-independent ConversationEngine API.
class PipelineConversationEngine final : public ConversationEngine {
 public:
  PipelineConversationEngine(AsrProvider& asr, AsyncSession& session,
                             TtsProvider& tts,
                             Conversation::TranscriptSink transcript_sink,
                             Conversation::AudioSink audio_sink,
                             ConversationConfig config = {});

  void start(ConversationEngineContext context) override;
  bool push_audio(AudioFrame frame) override;
  void handle_event(const Event& event) override;
  void stop() override;

  ConversationCapabilities capabilities() const override;
  ConversationStateSnapshot state() const override;

 private:
  void on_event(const Event& event);

  AsrProvider& asr_;
  AsyncSession& session_;
  TtsProvider& tts_;
  Conversation::TranscriptSink transcript_sink_;
  Conversation::AudioSink audio_sink_;
  ConversationConfig config_;

  ConversationEngineContext context_;
  std::unique_ptr<Conversation> conversation_;
  EventBus bridge_bus_;
  EventBus::Subscription bridge_subscription_ = 0;

  mutable std::mutex state_mutex_;
  ConversationStateSnapshot state_;
};

}  // namespace byteturn
