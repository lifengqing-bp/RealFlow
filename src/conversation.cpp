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
  alive_->store(false, std::memory_order_release);
  session_.cancel();
  tts_.cancel();
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    stopping_ = true;
    reset_asr_ = true;
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
  if (state_.load(std::memory_order_acquire) == ConversationState::Speaking)
    interrupt();
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
  ++generation_;
  session_.cancel();
  tts_.cancel();
  state_ = ConversationState::Listening;
  std::string turn_id;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    turn_id = turn_id_;
  }
  {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    reset_asr_ = true;
    audio_queue_.clear();
  }
  audio_cv_.notify_one();
  if (events_) events_->publish(
      {EventType::TurnCancelled, session_id_, turn_id, 0, {}, "barge_in", {}});
}

void Conversation::audio_loop() {
  const auto life = alive_;
  while (true) {
    AudioFrame frame;
    bool reset = false;
    {
      std::unique_lock<std::mutex> lock(audio_mutex_);
      audio_cv_.wait(lock, [this] {
        return stopping_ || reset_asr_ || !audio_queue_.empty();
      });
      if (stopping_) break;
      if (reset_asr_) {
        reset = true;
        reset_asr_ = false;
      }
      if (!audio_queue_.empty()) {
        frame = std::move(audio_queue_.front());
        audio_queue_.pop_front();
      } else if (!reset) {
        continue;
      }
    }
    if (reset) asr_.reset();
    if (frame.samples.empty() && !frame.end_of_utterance) continue;

    state_ = ConversationState::Listening;
    if (frame.end_of_utterance) {
      std::string turn_id;
      {
        std::lock_guard<std::mutex> lock(turn_mutex_);
        turn_id_ = std::to_string(
            next_turn_id_.fetch_add(1, std::memory_order_relaxed));
        turn_id = turn_id_;
      }
      if (events_) events_->publish(
          {EventType::AsrEndOfUtterance, session_id_, turn_id, 0, {}, {}, {}});
    }
    const auto callback_generation = generation_.load(std::memory_order_acquire);
    asr_.push(frame, [this, life, callback_generation](std::string text, bool final) {
      if (!life->load(std::memory_order_acquire)) return;
      on_transcript(std::move(text), final, callback_generation);
    });
  }
}

void Conversation::on_transcript(std::string text, bool is_final,
                                 std::uint64_t callback_generation) {
  if (callback_generation != generation_.load(std::memory_order_acquire)) return;
  transcript_sink_(text, is_final);
  std::string active_turn_id;
  {
    std::lock_guard<std::mutex> lock(turn_mutex_);
    if (is_final && !text.empty() && turn_id_.empty())
      turn_id_ = std::to_string(
          next_turn_id_.fetch_add(1, std::memory_order_relaxed));
    active_turn_id = turn_id_;
  }
  if (events_) events_->publish(
      {is_final ? EventType::TranscriptFinal : EventType::TranscriptPartial,
       session_id_, active_turn_id, 0, {}, {}, text});
  if (!is_final || text.empty()) return;

  state_ = ConversationState::Thinking;
  const auto my_generation = generation_.load(std::memory_order_acquire);
  const auto life = alive_;
  auto turn = std::make_shared<SpeechTurn>();
  const auto first_audio = turn->first_audio;
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

  const auto speak = [this, life, turn, active_turn_id,
                      my_generation](std::string sentence) {
    if (!life->load(std::memory_order_acquire) ||
        my_generation != generation_.load(std::memory_order_acquire))
      return false;
    if (!turn->speech_started) {
      turn->speech_started = true;
      state_ = ConversationState::Speaking;
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
          turn->speech->finish();
          if (!life->load(std::memory_order_acquire)) return;
          if (events_) {
            events_->publish(
                {EventType::SpeechCompleted, session_id_, active_turn_id,
                 0, {}, {}, {}});
            events_->publish(
                {EventType::ConversationTurnCompleted, session_id_, active_turn_id,
                 0, {}, {}, {}});
          }
          if (my_generation == generation_.load(std::memory_order_acquire))
            state_ = ConversationState::Listening;
          std::lock_guard<std::mutex> lock(turn_mutex_);
          if (turn_id_ == active_turn_id) turn_id_.clear();
        },
        [this, life, turn, active_turn_id](const TurnContext&,
                                           std::exception_ptr) {
          turn->speech->cancel();
          if (!life->load(std::memory_order_acquire)) return;
          state_ = ConversationState::Listening;
          std::lock_guard<std::mutex> lock(turn_mutex_);
          if (turn_id_ == active_turn_id) turn_id_.clear();
        },
        std::move(options));
    std::lock_guard<std::mutex> lock(handles_mutex_);
    active_turns_.push_back(handle.result);
  } catch (const ExecutorOverloaded& e) {
    turn->speech->cancel();
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
