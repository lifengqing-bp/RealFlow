#include "byteturn/full_duplex_conversation.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace byteturn {

FullDuplexConversation::FullDuplexConversation(
    RealtimeSpeechProvider& provider, std::string session_id,
    TranscriptSink transcript_sink, AudioSink audio_sink, EventBus* events,
    FullDuplexConfig config)
    : provider_(provider), session_id_(std::move(session_id)),
      transcript_sink_(std::move(transcript_sink)),
      audio_sink_(std::move(audio_sink)), events_(events), config_(config) {
  if (session_id_.empty()) throw std::invalid_argument("session id is required");
  if (!audio_sink_) throw std::invalid_argument("audio sink is required");
  if (config_.max_input_frames == 0 || config_.input_sample_rate_hz <= 0 ||
      config_.output_sample_rate_hz <= 0)
    throw std::invalid_argument("invalid full-duplex configuration");

  RealtimeSessionConfig provider_config;
  provider_config.session_id = session_id_;
  provider_config.input_sample_rate_hz = config_.input_sample_rate_hz;
  provider_config.output_sample_rate_hz = config_.output_sample_rate_hz;
  provider_config.server_vad = config_.server_vad;
  provider_session_ = provider_.connect(
      provider_config,
      [this](RealtimeEvent event) { on_provider_event(std::move(event)); });
  if (!provider_session_)
    throw std::runtime_error("realtime provider returned no session");
  input_worker_ = std::thread([this] { input_loop(); });
}

FullDuplexConversation::~FullDuplexConversation() { close(); }

bool FullDuplexConversation::push_audio(AudioFrame frame) {
  if (stopping_.load(std::memory_order_acquire)) return false;
  bool overloaded = false;
  {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (stopping_.load(std::memory_order_relaxed)) return false;
    if (input_queue_.size() >= config_.max_input_frames) {
      overloaded = true;
    } else {
      input_queue_.push_back(std::move(frame));
    }
  }
  if (overloaded) {
    publish(EventType::Error, {}, "audio_overload",
            "full-duplex input queue is full");
    return false;
  }
  input_cv_.notify_one();
  return true;
}

void FullDuplexConversation::input_speech_started() {
  if (stopping_.load(std::memory_order_acquire)) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!input_speech_active_) {
      input_speech_active_ = true;
      changed = true;
    }
  }
  if (!changed) return;
  publish(EventType::InputSpeechStarted);
  begin_barge_in();
}

void FullDuplexConversation::input_speech_ended() {
  if (stopping_.load(std::memory_order_acquire)) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (input_speech_active_) {
      input_speech_active_ = false;
      changed = true;
    }
  }
  if (changed) publish(EventType::InputSpeechEnded);
  provider_session_->commit_input();
}

void FullDuplexConversation::playback_started(const std::string& response_id) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (response_active_ && response_id == response_id_ && !playback_started_) {
      playback_started_ = true;
      changed = true;
    }
  }
  if (changed) publish(EventType::PlaybackStarted, response_id);
}

void FullDuplexConversation::acknowledge_playback(
    const std::string& response_id, std::uint64_t played_samples) {
  std::uint64_t accepted = 0;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (response_active_ && response_id == response_id_) {
      accepted = std::min(played_samples, delivered_samples_);
      if (accepted > played_samples_) {
        played_samples_ = accepted;
        if (response_generation_done_ && played_samples_ == delivered_samples_)
          response_active_ = false;
        changed = true;
      }
    }
  }
  if (changed)
    publish(EventType::PlaybackProgress, response_id, {},
            std::to_string(accepted));
}

void FullDuplexConversation::close() {
  if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
  input_cv_.notify_all();
  if (input_worker_.joinable()) input_worker_.join();
  if (provider_session_) provider_session_->close();
  publish(EventType::RealtimeSessionClosed);
}

void FullDuplexConversation::input_loop() {
  while (true) {
    AudioFrame frame;
    {
      std::unique_lock<std::mutex> lock(input_mutex_);
      input_cv_.wait(lock, [this] {
        return stopping_.load(std::memory_order_acquire) || !input_queue_.empty();
      });
      if (stopping_.load(std::memory_order_relaxed)) {
        input_queue_.clear();
        return;
      }
      frame = std::move(input_queue_.front());
      input_queue_.pop_front();
    }
    if (!provider_session_->push_audio(frame))
      publish(EventType::Error, {}, "provider_backpressure",
              "realtime provider rejected input audio");
    if (frame.end_of_utterance && !config_.server_vad)
      input_speech_ended();
  }
}

void FullDuplexConversation::on_provider_event(RealtimeEvent event) {
  if (stopping_.load(std::memory_order_acquire)) return;
  switch (event.type) {
    case RealtimeEventType::Connected:
      publish(EventType::RealtimeSessionStarted);
      return;
    case RealtimeEventType::InputSpeechStarted:
      input_speech_started();
      return;
    case RealtimeEventType::InputSpeechEnded: {
      bool changed = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        changed = input_speech_active_;
        input_speech_active_ = false;
      }
      if (changed) publish(EventType::InputSpeechEnded);
      return;
    }
    case RealtimeEventType::TranscriptPartial:
    case RealtimeEventType::TranscriptFinal: {
      const bool final = event.type == RealtimeEventType::TranscriptFinal;
      if (transcript_sink_) transcript_sink_(event.text, final);
      publish(final ? EventType::TranscriptFinal : EventType::TranscriptPartial,
              event.response_id, {}, event.text);
      return;
    }
    case RealtimeEventType::ResponseStarted: {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        response_id_ = event.response_id;
        delivered_samples_ = 0;
        played_samples_ = 0;
        response_active_ = true;
        response_generation_done_ = false;
        playback_started_ = false;
      }
      publish(EventType::SpeechStarted, event.response_id);
      return;
    }
    case RealtimeEventType::AudioDelta: {
      DuplexAudio output;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!response_active_ || response_generation_done_ ||
            event.response_id != response_id_) return;
        output.response_id = event.response_id;
        output.start_sample = delivered_samples_;
        output.frame = std::move(event.audio);
        delivered_samples_ += output.frame.samples.size();
      }
      if (!audio_sink_(output)) {
        begin_barge_in();
        return;
      }
      publish(EventType::AudioOutput, output.response_id, {},
              std::to_string(output.frame.samples.size()));
      return;
    }
    case RealtimeEventType::ResponseCompleted: {
      bool current = false;
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current = response_active_ && !response_generation_done_ &&
                  event.response_id == response_id_;
        if (current) {
          response_generation_done_ = true;
          // Generated audio may still be buffered by the player. Keep its
          // identity/offset available for a later acknowledgement or barge-in.
          response_active_ = played_samples_ < delivered_samples_;
        }
      }
      if (current) {
        publish(EventType::SpeechCompleted, event.response_id);
        publish(EventType::ConversationTurnCompleted, event.response_id);
      }
      return;
    }
    case RealtimeEventType::Error:
      publish(EventType::Error, event.response_id, event.error_code, event.text);
      return;
    case RealtimeEventType::Closed:
      publish(EventType::RealtimeSessionClosed);
      return;
  }
}

void FullDuplexConversation::begin_barge_in() {
  std::string response_id;
  std::uint64_t played_samples = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!response_active_) return;
    response_id = response_id_;
    played_samples = played_samples_;
    response_active_ = false;
  }
  publish(EventType::BargeInDetected, response_id);
  provider_session_->cancel_response(response_id, played_samples,
                                     config_.output_sample_rate_hz);
  publish(EventType::OutputCancelled, response_id, {},
          std::to_string(played_samples));
}

void FullDuplexConversation::publish(EventType type,
                                     const std::string& response_id,
                                     std::string name, std::string data) {
  if (events_)
    events_->publish({type, session_id_, response_id, 0, {}, std::move(name),
                      std::move(data), {}});
}

}  // namespace byteturn
