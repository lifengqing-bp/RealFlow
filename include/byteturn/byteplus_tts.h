#pragma once

#include "byteturn/openai_compatible.h"

#include <atomic>

namespace byteturn {

struct BytePlusTtsConfig {
  std::string api_key;
  // Choose a voice enabled for this resource in your BytePlus account.
  std::string speaker;
  std::string resource_id = "seed-tts-2.0";
  int sample_rate_hz = 24000;
  std::size_t max_text_bytes = 16 * 1024;
  std::size_t max_message_bytes = 1024 * 1024;
  std::size_t max_response_bytes = 8 * 1024 * 1024;
};

// Official BytePlus HTTP streaming TTS; outputs mono signed 16-bit PCM.
// The transport must outlive this provider, synchronously drain body_sink before
// returning, and enforce deadlines. Configure CurlHttpTransport.max_attempts=1
// to avoid resubmitting a potentially billed request after a network failure.
// Serialize begin/synthesize/end. Only cancel may run concurrently; drain calls
// before destruction. An application callback already running may finish.
class BytePlusTts final : public TtsProvider {
 public:
  BytePlusTts(BytePlusTtsConfig config, HttpTransport& transport);
  void begin_utterance() override;
  void cancel() override;
  void synthesize(const std::string& text,
                  const std::function<bool(const AudioFrame&)>& on_audio) override;
  void synthesize_chunk(const std::string& text,
                        const std::function<bool(const AudioFrame&)>& on_audio) override;

 private:
  const BytePlusTtsConfig config_;
  HttpTransport& transport_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace byteturn
