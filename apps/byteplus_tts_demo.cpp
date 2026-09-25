#include "byteturn/byteplus_tts.h"
#include "byteturn/curl_transport.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: byteplus_tts_demo 'text' output.pcm\n"
              << "Set BYTEPLUS_API_KEY and BYTEPLUS_TTS_SPEAKER; optionally BYTEPLUS_TTS_RESOURCE_ID.\n";
    return 2;
  }
  try {
    byteturn::BytePlusTtsConfig config;
    if (const char* v = std::getenv("BYTEPLUS_API_KEY")) config.api_key = v;
    if (const char* v = std::getenv("BYTEPLUS_TTS_SPEAKER")) config.speaker = v;
    if (const char* v = std::getenv("BYTEPLUS_TTS_RESOURCE_ID")) config.resource_id = v;
    byteturn::CurlTransportConfig http;
    http.max_attempts = 1;
    byteturn::CurlHttpTransport transport(http);
    byteturn::BytePlusTts tts(config, transport);
    std::ifstream existing(argv[2], std::ios::binary);
    if (existing.good()) throw std::runtime_error("output already exists");
    std::ofstream output(argv[2], std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output");
    tts.synthesize(argv[1], [&](const byteturn::AudioFrame& frame) {
      for (auto sample : frame.samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        output.put(static_cast<char>(value & 255));
        output.put(static_cast<char>(value >> 8));
      }
      return static_cast<bool>(output);
    });
    output.flush();
    if (!output) throw std::runtime_error("cannot flush output");
    std::cout << "Saved PCM: signed 16-bit little-endian, mono, 24000 Hz\n";
  } catch (const std::exception&) {
    std::cerr << "BytePlus synthesis failed; check credentials, voice/resource and connectivity.\n"
              << "Any output file may contain partial audio.\n";
    return 1;
  }
}
