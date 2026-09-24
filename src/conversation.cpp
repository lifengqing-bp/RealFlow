#include "byteturn/conversation.h"
#include "byteturn/incremental_tts.h"
#include "byteturn/sentence_segmenter.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace byteturn {
namespace {

struct SpeechTurn {
  SentenceSegmenter segmenter;
  std::unique_ptr<IncrementalTtsPipeline> speech;
  std::shared_ptr<std::atomic<bool>> first_audio =
      std::make_shared<std::atomic<bool>>(true);
  bool speech_started = false;  // Accessed only by the session executor lane.
};

}  // namespace

Conversation::Conversation(AsrProvider& asr, AsyncSession& session,
                           TtsProvider& tts, TranscriptSink transcript_sink,
                           AudioSink audio_sink, EventBus* events,
                           std::string session_id, ConversationConfig config)
    : asr_(asr), session_(session), tts_(tts),
      transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)), events_(events),
      session_id_(session_id.empty() ? session.id() : std::move(session_id)),
      config_(config), alive_(std::make_shared<std::atomic<bool>>(true)) {
  if (config_.max_audio_frames == 0 || config_.max_tts_chunks == 0 ||
      config_.turn_timeout.count() <= 0)
    throw std::invalid_argument("invalid conversation queue or timeout configuration");
  audio_worker_ = std::thread([this] { audio_loop(); });
}

Conversation::~Conversation() {
  {
    std::lock_guard<std::mutex> submit(submission_mutex_);
    alive_->store(false, std::memory_order_release);
    ++generation_;
    session_.cancel();
    tts_.cancel();
  }
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    stopping_ = true;
    audio_queue_.clear();
  }
  asr_.reset();
  audio_cv_.notify_all();
  if (audio_worker_.joinable()) audio_worker_.join();

  std::vector<std::shared_future<std::string>> turns;
  {
    std::lock_guard<std::mutex> lock(handles_mutex_);
    turns.swap(active_turns_);
  }
  for (auto& turn : turns) {
    try {
      turn.wait();
    } catch (...) {
    }
  }
}

bool Conversation::push_audio(const AudioFrame& frame) {
  if (!alive_->load(std::memory_order_acquire)) return false;
  bool overloaded = false;
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (stopping_) return false;
    if (audio_queue_.size() >= config_.max_audio_frames) {
      overloaded = true;
    } else {
      audio_queue_.push_back(frame);
    }
  }
  if (overloaded) {
    if (events_) events_->publish(
        {EventType::Error, session_id_, {}, 0, {}, "audio_overload",
         "audio input queue is full"});
    return false;
  }
  audio_cv_.notify_one();
  reap_turns();
  return true;
}

void Conversation::interrupt() {
  // Record receipt before signalling workers; never call subscribers under a
  // control lock. This request is session-scoped, not a terminal outcome.
  if (events_) events_->publish(
      {EventType::ResponseCancelRequested, session_id_, {}, 0, {},
       "all_responses", {}});
  {
    std::lock_guard<std::mutex> submit(submission_mutex_);
    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      ++generation_;
      state_ = ConversationState::Listening;
      turn_id_.clear();
    }
    session_.cancel();
    tts_.cancel();
  }
  // Input may already contain the correction. Neither clear its queue nor
  // reset the recognizer; response cancellation is not input cancellation.
}

void Conversation::audio_loop() {
  const auto life = alive_;
  while (true) {
    AudioFrame frame;
    {
      std::unique_lock<std::mutex> lock(audio_mutex_);
      audio_cv_.wait(lock, [this] { return stopping_ || !audio_queue_.empty(); });
      if (stopping_) break;
      frame = std::move(audio_queue_.front());
      audio_queue_.pop_front();
    }
    if (frame.samples.empty() && !frame.end_of_utterance) continue;

    std::string input_turn_id;
    if (frame.end_of_utterance) {
      input_turn_id = std::to_string(
          next_turn_id_.fetch_add(1, std::memory_order_relaxed));
      if (events_) events_->publish(
          {EventType::AsrEndOfUtterance, session_id_, input_turn_id, 0, {}, {}, {}});
    }
    // ASR callbacks belong to input, not the generation being cancelled.
    try {
      asr_.push(frame, [this, life, input_turn_id](std::string text, bool final) {
        if (!life->load(std::memory_order_acquire)) return;
        on_transcript(std::move(text), final, input_turn_id);
      });
    } catch (...) {
      // Provider I/O failure must not escape the audio worker and terminate the
      // process. Keep input available for a later request/reconnect. Do not log
      // provider exception text, which may contain payloads or credentials.
      if (life->load(std::memory_order_acquire) && events_) {
        try { events_->publish({EventType::Error, session_id_, input_turn_id,
                               0, {}, "asr", "ASR input failed"}); }
        catch (...) {} // Observation failure cannot terminate the worker either.
      }
    }
  }
}

void Conversation::on_transcript(std::string text, bool is_final,
                                 std::string input_turn_id) {
  transcript_sink_(text, is_final);
  if (is_final && !text.empty() && input_turn_id.empty())
    input_turn_id = std::to_string(
        next_turn_id_.fetch_add(1, std::memory_order_relaxed));
  if (events_) events_->publish(
      {is_final ? EventType::TranscriptFinal : EventType::TranscriptPartial,
       session_id_, input_turn_id, 0, {}, {}, text});
  if (!is_final || text.empty()) return;

  std::unique_lock<std::mutex> submit(submission_mutex_);
  if (!alive_->load(std::memory_order_acquire)) return;
  const auto my_generation = generation_.load(std::memory_order_acquire);
  const auto active_turn_id = std::move(input_turn_id);
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    state_ = ConversationState::Thinking;
    turn_id_ = active_turn_id;
  }
  const auto life = alive_;
  auto turn = std::make_shared<SpeechTurn>();
  const auto first_audio = turn->first_audio;
  const auto start_speech = [this, life, turn, my_generation,
                             active_turn_id, first_audio] {
    turn->speech = std::make_unique<IncrementalTtsPipeline>(
      tts_,
      [this, life, my_generation, active_turn_id, first_audio](const AudioFrame& frame) {
        if (!life->load(std::memory_order_acquire) ||
            my_generation != generation_.load(std::memory_order_acquire))
          return false;
        if (first_audio->exchange(false, std::memory_order_acq_rel) && events_)
          events_->publish(
              {EventType::FirstAudio, session_id_, active_turn_id, 0, {}, {}, {}});
        audio_sink_(frame);
        if (events_) events_->publish(
            {EventType::AudioOutput, session_id_, active_turn_id, 0, {}, {},
             std::to_string(frame.samples.size())});
        return true;
      },
      [this, life, my_generation] {
        return !life->load(std::memory_order_acquire) ||
               my_generation != generation_.load(std::memory_order_acquire);
      },
      config_.max_tts_chunks);
  };

  const auto speak = [this, life, turn, active_turn_id, start_speech,
                      my_generation](std::string sentence) {
    if (!life->load(std::memory_order_acquire) ||
        my_generation != generation_.load(std::memory_order_acquire))
      return false;
    if (!turn->speech_started) {
      {
        std::lock_guard<std::mutex> lock(turn_mutex_);
        if (my_generation != generation_.load(std::memory_order_acquire))
          return false;
        state_ = ConversationState::Speaking;
      }
      turn->speech_started = true;
      start_speech();
      if (events_) events_->publish(
          {EventType::SpeechStarted, session_id_, active_turn_id, 0, {}, {}, {}});
    }
    if (events_) events_->publish(
        {EventType::TtsChunkStarted, session_id_, active_turn_id, 0, {}, {},
         sentence});
    return turn->speech->push(std::move(sentence));
  };

  TurnOptions options;
  options.turn_id = active_turn_id;
  options.trace_id = session_id_ + ":" + active_turn_id;
  options.timeout = config_.turn_timeout;
  try {
    TurnHandle handle = session_.submit_streaming(
        std::move(text),
        [turn, speak](const std::string& delta) {
          for (auto& sentence : turn->segmenter.push(delta))
            if (!speak(std::move(sentence))) return false;
          return true;
        },
        [this, life, turn, speak, active_turn_id,
         my_generation](const TurnContext& context, const std::string&) {
          context.transition(TurnState::Synthesizing);
          for (auto& sentence : turn->segmenter.flush())
            if (!speak(std::move(sentence))) break;
          if (turn->speech) turn->speech->finish();
          if (context.stop_requested() ||
              !life->load(std::memory_order_acquire) ||
              my_generation != generation_.load(std::memory_order_acquire))
            throw std::runtime_error("response cancelled or deadline exceeded");
          if (events_) {
            if (turn->speech_started) events_->publish(
                {EventType::SpeechCompleted, session_id_, active_turn_id,
                 0, {}, {}, {}});
            events_->publish(
                {EventType::ConversationTurnCompleted, session_id_, active_turn_id,
                 0, {}, {}, {}});
          }
          std::lock_guard<std::mutex> lock(turn_mutex_);
          if (my_generation == generation_.load(std::memory_order_acquire) &&
              turn_id_ == active_turn_id) {
            state_ = ConversationState::Listening;
            turn_id_.clear();
          }
        },
        [this, life, turn, active_turn_id, my_generation](const TurnContext& context,
                                                          std::exception_ptr) {
          if (turn->speech) {
            turn->speech->cancel();
            // Quiesce this response's TTS before the shared provider is reused
            // by the next executor task. Preserve the original failure.
            try { turn->speech->finish(); } catch (...) {}
          }
          if (events_) events_->publish(
              {context.expired() ? EventType::ConversationTurnFailed
                   : context.cancelled() ||
                     my_generation != generation_.load(std::memory_order_acquire)
                       ? EventType::ConversationTurnCancelled
                       : EventType::ConversationTurnFailed,
               session_id_, active_turn_id, 0, {},
               context.expired() ? "deadline" : "response_terminated", {}});
          if (!life->load(std::memory_order_acquire)) return;
          std::lock_guard<std::mutex> lock(turn_mutex_);
          if (my_generation == generation_.load(std::memory_order_acquire) &&
              turn_id_ == active_turn_id) {
            state_ = ConversationState::Listening;
            turn_id_.clear();
          }
        },
        std::move(options), events_);
    std::lock_guard<std::mutex> lock(handles_mutex_);
    active_turns_.push_back(handle.result);
  } catch (const ExecutorOverloaded& e) {
    submit.unlock();
    state_ = ConversationState::Listening;
    {
      std::lock_guard<std::mutex> lock(turn_mutex_);
      if (turn_id_ == active_turn_id) turn_id_.clear();
    }
    if (events_) events_->publish(
        {EventType::Error, session_id_, active_turn_id, 0, {}, "turn_overload",
         e.what()});
  }
}

void Conversation::reap_turns() {
  std::lock_guard<std::mutex> lock(handles_mutex_);
  active_turns_.erase(
      std::remove_if(active_turns_.begin(), active_turns_.end(),
                     [](std::shared_future<std::string>& future) {
                       if (future.wait_for(std::chrono::milliseconds(0)) !=
                           std::future_status::ready)
                         return false;
                       try {
                         (void)future.get();
                       } catch (...) {
                       }
                       return true;
                     }),
      active_turns_.end());
}

}  // namespace byteturn
