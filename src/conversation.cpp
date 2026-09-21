#include "byteturn/conversation.h"

#include <utility>

namespace byteturn {

Conversation::Conversation(AsrProvider& asr, Agent& agent, TtsProvider& tts,
                           TranscriptSink transcript_sink, AudioSink audio_sink)
    : asr_(asr), agent_(agent), tts_(tts),
      transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)) {}

void Conversation::push_audio(const AudioFrame& frame) {
  // Incoming speech while speaking is treated as barge-in.
  if (state_ == ConversationState::Speaking) interrupt();
  state_ = ConversationState::Listening;
  asr_.push(frame, [this](std::string text, bool final) {
    on_transcript(std::move(text), final);
  });
}

void Conversation::interrupt() {
  ++generation_;
  tts_.cancel();
  asr_.reset();
  state_ = ConversationState::Listening;
}

void Conversation::on_transcript(std::string text, bool is_final) {
  transcript_sink_(text, is_final);
  if (!is_final || text.empty()) return;

  state_ = ConversationState::Thinking;
  const std::string reply = agent_.run(std::move(text));
  state_ = ConversationState::Speaking;
  const auto my_generation = generation_;
  tts_.synthesize(reply, [this, my_generation](const AudioFrame& frame) {
    if (my_generation != generation_) return false;
    audio_sink_(frame);
    return true;
  });
  if (my_generation == generation_) state_ = ConversationState::Listening;
}

}  // namespace byteturn

