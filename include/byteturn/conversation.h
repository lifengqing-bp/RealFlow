#pragma once

#include "byteturn/session.h"

#include <condition_variable>
#include <chrono>
#include <deque>
#include <future>
#include <functional>
#include <atomic>
#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace byteturn {

enum class ConversationState { Listening, Thinking, Speaking };

struct ConversationConfig {
  std::size_t max_audio_frames = 100;
  std::chrono::milliseconds turn_timeout{15000};
  std::size_t max_tts_chunks = 4;
};

class Conversation {
 public:
  using TranscriptSink = std::function<void(const std::string&, bool)>;
  using AudioSink = std::function<void(const AudioFrame&)>;

  Conversation(AsrProvider& asr, AsyncSession& session, TtsProvider& tts,
               TranscriptSink transcript_sink, AudioSink audio_sink,
               EventBus* events = nullptr, std::string session_id = {},
               ConversationConfig config = {});
  ~Conversation();
  Conversation(const Conversation&) = delete;
  Conversation& operator=(const Conversation&) = delete;
  bool push_audio(const AudioFrame& frame);
  void interrupt();
  ConversationState state() const { return state_.load(std::memory_order_acquire); }

 private:
  void on_transcript(std::string text, bool is_final,
                     std::uint64_t callback_generation);
  void audio_loop();
  void reap_turns();

  AsrProvider& asr_;
  AsyncSession& session_;
  TtsProvider& tts_;
  TranscriptSink transcript_sink_;
  AudioSink audio_sink_;
  std::atomic<ConversationState> state_{ConversationState::Listening};
  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> next_turn_id_{1};
  EventBus* events_;
  std::string session_id_;
  ConversationConfig config_;
  mutable std::mutex turn_mutex_;
  std::string turn_id_;
  std::mutex audio_mutex_;
  std::condition_variable audio_cv_;
  std::deque<AudioFrame> audio_queue_;
  std::thread audio_worker_;
  bool reset_asr_ = false;
  bool stopping_ = false;
  std::mutex handles_mutex_;
  std::vector<std::shared_future<std::string>> active_turns_;
  std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace byteturn
