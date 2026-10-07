#include "byteturn/native_duplex_conversation_engine.h"

#include <stdexcept>
#include <utility>

namespace byteturn {

NativeDuplexConversationEngine::NativeDuplexConversationEngine(
    RealtimeSpeechProvider& provider,
    FullDuplexConversation::TranscriptSink transcript_sink,
    FullDuplexConversation::AudioSink audio_sink, FullDuplexConfig config)
    : provider_(provider), transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)), config_(config) {}

NativeDuplexConversationEngine::~NativeDuplexConversationEngine() { stop(); }

void NativeDuplexConversationEngine::start(ConversationEngineContext context) {
  if (conversation_) return;
  if (!context.emit && context.timeline) context.emit = context.timeline->sink();
  if (!context.emit) throw std::invalid_argument("event sink is required");
  // Private bridge only: native observations have exactly one canonical ingress.
  bridge_subscription_ = bridge_bus_.subscribe(
      [emit = std::move(context.emit)](const Event& event) { (void)emit(event); });
  conversation_ = std::make_unique<FullDuplexConversation>(
      provider_, std::move(context.session_id), transcript_sink_, audio_sink_,
      &bridge_bus_, config_);
}

bool NativeDuplexConversationEngine::push_audio(AudioFrame frame) {
  return conversation_ && conversation_->push_audio(std::move(frame));
}
bool NativeDuplexConversationEngine::cancel_response() {
  return conversation_ && conversation_->cancel_response();
}
bool NativeDuplexConversationEngine::playback_started(const std::string& id) {
  return conversation_ && conversation_->playback_started(id);
}
bool NativeDuplexConversationEngine::acknowledge_playback(
    const std::string& id, std::uint64_t samples) {
  return conversation_ && conversation_->acknowledge_playback(id, samples);
}
void NativeDuplexConversationEngine::handle_event(const Event&) {
  // Observation only. Playback and cancellation use explicit control methods.
}
void NativeDuplexConversationEngine::stop() {
  // Quiesce input and provider callbacks before releasing the bridge/sinks.
  conversation_.reset();
  if (bridge_subscription_) {
    bridge_bus_.unsubscribe(bridge_subscription_);
    bridge_subscription_ = 0;
  }
}
ConversationCapabilities NativeDuplexConversationEngine::capabilities() const {
  ConversationCapabilities value;
  value.native_full_duplex = true;
  value.simultaneous_listen_speak = true;
  value.native_interruption = config_.server_vad;
  value.native_end_of_utterance = config_.server_vad;
  value.response_cancellation = true;
  value.playback_acknowledgement = true;
  return value;
}
ConversationStateSnapshot NativeDuplexConversationEngine::state() const {
  ConversationStateSnapshot value;
  if (conversation_) {
    const auto native = conversation_->state();
    value.user_speaking = native.user_speaking;
    // Active generation or outstanding playback, not proof of physical audio.
    value.agent_speaking = native.response_active;
  }
  return value;
}

}  // namespace byteturn
