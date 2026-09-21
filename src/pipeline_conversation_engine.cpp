#include "byteturn/pipeline_conversation_engine.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

PipelineConversationEngine::PipelineConversationEngine(
    AsrProvider& asr, AsyncSession& session, TtsProvider& tts,
    Conversation::TranscriptSink transcript_sink,
    Conversation::AudioSink audio_sink, ConversationConfig config)
    : asr_(asr),
      session_(session),
      tts_(tts),
      transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)),
      config_(config) {}

void PipelineConversationEngine::start(ConversationEngineContext context) {
  if (conversation_) return;
  if (context.session_id != session_.id())
    throw std::invalid_argument("pipeline and conversation session IDs must match");
  if (!context.emit && context.timeline) context.emit = context.timeline->sink();
  if (!context.emit) throw std::invalid_argument("event sink is required");
  context_ = std::move(context);
  bridge_subscription_ =
      bridge_bus_.subscribe([this](const Event& event) { on_event(event); });
  conversation_ = std::make_unique<Conversation>(
      asr_, session_, tts_, transcript_sink_, audio_sink_, &bridge_bus_,
      context_.session_id, config_);
}

bool PipelineConversationEngine::push_audio(AudioFrame frame) {
  return conversation_ && conversation_->push_audio(frame);
}

bool PipelineConversationEngine::cancel_response() {
  if (!conversation_) return false;
  conversation_->interrupt();
  return true;
}

void PipelineConversationEngine::handle_event(const Event&) {
  // External semantic events are already recorded by ConversationSession.
  // The current pipeline has no provider-independent control events to consume.
}

void PipelineConversationEngine::stop() {
  conversation_.reset();
  if (bridge_subscription_ != 0) {
    bridge_bus_.unsubscribe(bridge_subscription_);
    bridge_subscription_ = 0;
  }
}

ConversationCapabilities PipelineConversationEngine::capabilities() const {
  ConversationCapabilities value;
  value.transcript_available = true;
  value.response_cancellation = true;
  return value;
}

ConversationStateSnapshot PipelineConversationEngine::state() const {
  // Read the pipeline's state directly; best-effort/out-of-order observations
  // must not restore an already cancelled response. These are generation flags,
  // not player acknowledgements. User speech is not inferred from raw PCM.
  ConversationStateSnapshot value;
  if (conversation_) {
    const auto phase = conversation_->state();
    value.agent_speaking = phase == ConversationState::Speaking;
    value.reasoning = phase == ConversationState::Thinking;
  }
  return value;
}

void PipelineConversationEngine::on_event(const Event& event) {
  (void)context_.emit(event);
}

}  // namespace byteturn
