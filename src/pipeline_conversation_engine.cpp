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
  if (!context.timeline) throw std::invalid_argument("event timeline is required");
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
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_ = {};
}

ConversationCapabilities PipelineConversationEngine::capabilities() const {
  ConversationCapabilities value;
  value.transcript_available = true;
  return value;
}

ConversationStateSnapshot PipelineConversationEngine::state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

void PipelineConversationEngine::on_event(const Event& event) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    switch (event.type) {
      case EventType::InputSpeechStarted:
        state_.user_speaking = true;
        break;
      case EventType::InputSpeechEnded:
        state_.user_speaking = false;
        break;
      case EventType::ModelStarted:
        state_.reasoning = true;
        break;
      case EventType::ModelCompleted:
      case EventType::TurnCancelled:
        state_.reasoning = false;
        break;
      case EventType::SpeechStarted:
        state_.agent_speaking = true;
        break;
      case EventType::SpeechCompleted:
      case EventType::OutputCancelled:
        state_.agent_speaking = false;
        break;
      case EventType::BargeInDetected:
        state_.interruption_pending = true;
        break;
      case EventType::ConversationTurnCompleted:
        state_.interruption_pending = false;
        break;
      default:
        break;
    }
  }
  context_.timeline->append(event);
}

}  // namespace byteturn
