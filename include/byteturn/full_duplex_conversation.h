#pragma once

#include "byteturn/event.h"
#include "byteturn/providers.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace byteturn {

struct DuplexAudio {
  std::string response_id;
  std::uint64_t start_sample = 0;
  AudioFrame frame;
};

struct FullDuplexConfig {
  std::size_t max_input_frames = 100;
  int input_sample_rate_hz = 16000;
  int output_sample_rate_hz = 24000;
  bool server_vad = true;
};

// Drives one native audio-in/audio-out model session. Input remains live while
// output is being generated or played. Barge-in cancellation is based on
// playback acknowledgements, so unheard provider audio is removed from model
// context rather than accidentally committed as heard speech.
class FullDuplexConversation {
 public:
  using TranscriptSink = std::function<void(const std::string&, bool)>;
  using AudioSink = std::function<bool(const DuplexAudio&)>;

  FullDuplexConversation(RealtimeSpeechProvider& provider,
                         std::string session_id,
                         TranscriptSink transcript_sink,
                         AudioSink audio_sink,
                         EventBus* events = nullptr,
                         FullDuplexConfig config = {});
  ~FullDuplexConversation();
  FullDuplexConversation(const FullDuplexConversation&) = delete;
  FullDuplexConversation& operator=(const FullDuplexConversation&) = delete;

  bool push_audio(AudioFrame frame);
  void input_speech_started();
  void input_speech_ended();

  // The transport/player must acknowledge what became audible. Values are
  // monotonic offsets within response_id at output_sample_rate_hz.
  void playback_started(const std::string& response_id);
  void acknowledge_playback(const std::string& response_id,
                            std::uint64_t played_samples);
  void close();

 private:
  void input_loop();
  void on_provider_event(RealtimeEvent event);
  void begin_barge_in();
  void publish(EventType type, const std::string& response_id = {},
               std::string name = {}, std::string data = {});

  RealtimeSpeechProvider& provider_;
  std::string session_id_;
  TranscriptSink transcript_sink_;
  AudioSink audio_sink_;
  EventBus* events_;
  FullDuplexConfig config_;
  std::unique_ptr<RealtimeSpeechSession> provider_session_;

  std::mutex input_mutex_;
  std::condition_variable input_cv_;
  std::deque<AudioFrame> input_queue_;
  std::thread input_worker_;

  std::mutex state_mutex_;
  std::string response_id_;
  std::uint64_t delivered_samples_ = 0;
  std::uint64_t played_samples_ = 0;
  bool response_active_ = false;
  bool playback_started_ = false;
  bool input_speech_active_ = false;
  std::atomic<bool> stopping_{false};
};

}  // namespace byteturn
