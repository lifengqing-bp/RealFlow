#include "byteturn/conversation.h"
#include "byteturn/incremental_tts.h"
#include "byteturn/sentence_segmenter.h"

#include <utility>

namespace byteturn {

Conversation::Conversation(AsrProvider& asr, Agent& agent, TtsProvider& tts,
                           TranscriptSink transcript_sink, AudioSink audio_sink,
                           EventBus* events, std::string session_id)
    : asr_(asr), agent_(agent), tts_(tts),
      transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)), events_(events),
      session_id_(std::move(session_id)) {}

void Conversation::push_audio(const AudioFrame& frame) {
  // Incoming speech while speaking is treated as barge-in.
  if (state_.load(std::memory_order_acquire) == ConversationState::Speaking) interrupt();
  state_ = ConversationState::Listening;
  if (frame.end_of_utterance) {
    std::string turn_id;
    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      turn_id_ = std::to_string(next_turn_id_.fetch_add(1, std::memory_order_relaxed));
      turn_id = turn_id_;
    }
    if (events_) events_->publish(
        {EventType::AsrEndOfUtterance, session_id_, turn_id, 0, {}, {}, {}});
  }
  asr_.push(frame, [this](std::string text, bool final) {
    on_transcript(std::move(text), final);
  });
}

void Conversation::interrupt() {
  ++generation_;
  tts_.cancel();
  asr_.reset();
  state_ = ConversationState::Listening;
  std::string turn_id;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    turn_id = turn_id_;
  }
  if (events_) events_->publish(
      {EventType::TurnCancelled, session_id_, turn_id, 0, {}, {}, {}});
}

void Conversation::on_transcript(std::string text, bool is_final) {
  transcript_sink_(text, is_final);
  std::string active_turn_id;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    if (is_final && !text.empty() && turn_id_.empty())
      turn_id_ = std::to_string(next_turn_id_.fetch_add(1, std::memory_order_relaxed));
    active_turn_id = turn_id_;
  }
  if (events_) events_->publish(
      {is_final ? EventType::TranscriptFinal : EventType::TranscriptPartial,
       session_id_, active_turn_id, 0, {}, {}, text});
  if (!is_final || text.empty()) return;

  state_ = ConversationState::Thinking;
  const auto my_generation = generation_.load(std::memory_order_acquire);
  SentenceSegmenter segmenter;
  bool speech_started = false;
  std::atomic<bool> first_audio{true};
  IncrementalTtsPipeline speech(
      tts_,
      [this, my_generation, active_turn_id, &first_audio](const AudioFrame& frame) {
        if (my_generation != generation_.load(std::memory_order_acquire)) return false;
        if (first_audio.exchange(false, std::memory_order_acq_rel) && events_)
          events_->publish(
              {EventType::FirstAudio, session_id_, active_turn_id, 0, {}, {}, {}});
        audio_sink_(frame);
        if (events_) events_->publish(
            {EventType::AudioOutput, session_id_, active_turn_id, 0, {}, {},
             std::to_string(frame.samples.size())});
        return true;
      },
      [this, my_generation] {
        return my_generation != generation_.load(std::memory_order_acquire);
      });
  const auto speak = [&](std::string sentence) {
    if (!speech_started) {
      speech_started = true;
      state_ = ConversationState::Speaking;
      if (events_) events_->publish(
          {EventType::SpeechStarted, session_id_, active_turn_id, 0, {}, {}, {}});
    }
    if (events_) events_->publish(
        {EventType::TtsChunkStarted, session_id_, active_turn_id, 0, {}, {}, sentence});
    return speech.push(std::move(sentence));
  };
  try {
    agent_.run_streaming(
        std::move(text), session_id_, active_turn_id,
        [&](const std::string& delta) {
          for (auto& sentence : segmenter.push(delta))
            if (!speak(std::move(sentence))) return false;
          return my_generation == generation_.load(std::memory_order_acquire);
        },
        [this, my_generation] {
          return my_generation != generation_.load(std::memory_order_acquire);
        });
    for (auto& sentence : segmenter.flush())
      if (!speak(std::move(sentence))) break;
    speech.finish();
    if (events_) {
      events_->publish(
          {EventType::SpeechCompleted, session_id_, active_turn_id, 0, {}, {}, {}});
      events_->publish(
          {EventType::ConversationTurnCompleted, session_id_, active_turn_id, 0, {}, {}, {}});
    }
  } catch (...) {
    speech.cancel();
    state_ = ConversationState::Listening;
    throw;
  }
  if (my_generation == generation_.load(std::memory_order_acquire))
    state_ = ConversationState::Listening;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    if (turn_id_ == active_turn_id) turn_id_.clear();
  }
}

}  // namespace byteturn
