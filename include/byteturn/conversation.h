#pragma once

#include "byteturn/agent.h"

#include <functional>
#include <string>

namespace byteturn {

enum class ConversationState { Listening, Thinking, Speaking };

class Conversation {
 public:
  using TranscriptSink = std::function<void(const std::string&, bool)>;
  using AudioSink = std::function<void(const AudioFrame&)>;

  Conversation(AsrProvider& asr, Agent& agent, TtsProvider& tts,
               TranscriptSink transcript_sink, AudioSink audio_sink,
               EventBus* events = nullptr, std::string session_id = {});
  void push_audio(const AudioFrame& frame);
  void interrupt();
  ConversationState state() const { return state_; }

 private:
  void on_transcript(std::string text, bool is_final);

  AsrProvider& asr_;
  Agent& agent_;
  TtsProvider& tts_;
  TranscriptSink transcript_sink_;
  AudioSink audio_sink_;
  ConversationState state_ = ConversationState::Listening;
  std::uint64_t generation_ = 0;
  EventBus* events_;
  std::string session_id_;
  std::string turn_id_;
};

}  // namespace byteturn
