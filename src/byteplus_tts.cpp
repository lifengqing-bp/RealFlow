#include "byteturn/byteplus_tts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace byteturn {
namespace {
using Json = nlohmann::json;

bool valid_header(const std::string& s) {
  return !s.empty() && s.size() <= 4096 &&
      std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 33 && c <= 126; });
}

BytePlusTtsConfig checked(BytePlusTtsConfig c) {
  const int rates[] = {8000, 16000, 22050, 24000, 32000, 44100, 48000};
  if (!valid_header(c.api_key) || !valid_header(c.resource_id) ||
      c.speaker.empty() || c.speaker.size() > 256 ||
      std::find(std::begin(rates), std::end(rates), c.sample_rate_hz) == std::end(rates) ||
      !c.max_text_bytes || c.max_text_bytes > 1024 * 1024 ||
      !c.max_message_bytes || c.max_message_bytes > 8 * 1024 * 1024 ||
      !c.max_response_bytes || c.max_response_bytes > 64 * 1024 * 1024)
    throw std::invalid_argument("invalid BytePlus TTS configuration");
  return c;
}

int sextet(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  throw std::runtime_error("invalid BytePlus audio base64");
}

std::string decode(const std::string& s) {
  if (s.size() % 4) throw std::runtime_error("invalid BytePlus audio base64 length");
  std::string result;
  result.reserve(s.size() / 4 * 3);
  for (std::size_t i = 0; i < s.size(); i += 4) {
    const unsigned a = sextet(s[i]), b = sextet(s[i + 1]);
    const bool pad2 = s[i + 2] == '=', pad3 = s[i + 3] == '=';
    const unsigned c = pad2 ? 0 : sextet(s[i + 2]);
    const unsigned d = pad3 ? 0 : sextet(s[i + 3]);
    if ((pad2 && !pad3) || ((pad2 || pad3) && i + 4 != s.size()) ||
        (pad2 && (b & 15)) || (pad3 && !pad2 && (c & 3)))
      throw std::runtime_error("invalid BytePlus audio base64 padding");
    result.push_back(static_cast<char>((a << 2) | (b >> 4)));
    if (!pad2) result.push_back(static_cast<char>((b << 4) | (c >> 2)));
    if (!pad3) result.push_back(static_cast<char>((c << 6) | d));
  }
  return result;
}

bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}  // namespace

BytePlusTts::BytePlusTts(BytePlusTtsConfig config, HttpTransport& transport)
    : config_(checked(std::move(config))), transport_(transport) {}
void BytePlusTts::begin_utterance() { cancelled_.store(false); }
void BytePlusTts::cancel() { cancelled_.store(true); }
void BytePlusTts::synthesize(const std::string& text,
    const std::function<bool(const AudioFrame&)>& sink) {
  begin_utterance();
  synthesize_chunk(text, sink);
}

void BytePlusTts::synthesize_chunk(const std::string& text,
    const std::function<bool(const AudioFrame&)>& sink) {
  const auto check = [&] {
    if (cancelled_.load()) throw std::runtime_error("BytePlus TTS cancelled");
  };
  check();
  if (!sink || text.empty() || text.size() > config_.max_text_bytes)
    throw std::invalid_argument("invalid BytePlus TTS input");
  HttpRequest request;
  request.method = "POST";
  request.url = "https://voice.ap-southeast-1.bytepluses.com/api/v3/tts/unidirectional";
  request.headers = {{"X-Api-Key", config_.api_key},
                     {"X-Api-Resource-Id", config_.resource_id},
                     {"X-Api-App-Key", "aGjiRDfUWi"},
                     {"Content-Type", "application/json"}};
  try {
    request.body = Json{{"req_params", {{"text", text}, {"speaker", config_.speaker},
        {"audio_params", {{"format", "pcm"}, {"sample_rate", config_.sample_rate_hz}}},
        {"additions", "{}"}}}}.dump();
  } catch (const Json::exception&) {
    throw std::invalid_argument("invalid BytePlus TTS input encoding");
  }
  request.cancelled = [&] { return cancelled_.load(); };
  std::string pending;
  std::size_t received = 0;
  int depth = 0, low_byte = -1;
  bool quoted = false, escaped = false, done = false;
  const auto consume = [&] {
    Json message;
    try {
      message = Json::parse(pending);
      if (!message.is_object() || !message.contains("code") ||
          !message["code"].is_number_integer())
        throw std::runtime_error("invalid BytePlus TTS response");
      const auto& code = message["code"];
      if (code == 20000000) {
        if (low_byte >= 0 || (message.contains("data") && !message["data"].is_null() &&
                            message["data"] != ""))
          throw std::runtime_error("invalid BytePlus TTS terminal response");
        done = true;
        return;
      }
      if (code != 0) throw std::runtime_error("BytePlus TTS service error");
      if (!message.contains("data") || message["data"].is_null()) return;
      const auto bytes = decode(message["data"].get<std::string>());
      AudioFrame frame;
      frame.sample_rate_hz = config_.sample_rate_hz;
      frame.channels = 1;
      frame.samples.reserve((bytes.size() + (low_byte >= 0)) / 2);
      for (unsigned char byte : bytes) {
        if (low_byte < 0) { low_byte = byte; continue; }
        const int value = low_byte | (static_cast<int>(byte) << 8);
        frame.samples.push_back(static_cast<std::int16_t>(value < 32768 ? value : value - 65536));
        low_byte = -1;
      }
      check();
      if (!frame.samples.empty() && !sink(frame))
        throw std::runtime_error("BytePlus TTS consumer stopped");
      check();
    } catch (const Json::exception&) {
      // JSON diagnostics may contain user text or supplier payloads.
      throw std::runtime_error("invalid BytePlus TTS JSON response");
    }
  };
  request.body_sink = [&](std::string_view bytes) {
    check();
    if (bytes.size() > config_.max_response_bytes - received)
      throw std::runtime_error("BytePlus TTS response limit");
    received += bytes.size();
    // HTTP chunk boundaries need not coincide with JSON records (or PCM samples).
    // Accept whitespace-separated/adjacent JSON objects, including pretty JSON.
    for (char c : bytes) {
      if (pending.empty() && whitespace(c)) continue;
      if (done) throw std::runtime_error("BytePlus TTS data after completion");
      if (pending.empty() && c != '{')
        throw std::runtime_error("invalid BytePlus TTS stream");
      if (pending.size() == config_.max_message_bytes)
        throw std::runtime_error("BytePlus TTS message limit");
      pending.push_back(c);
      if (quoted) {
        if (escaped) escaped = false;
        else if (c == '\\') escaped = true;
        else if (c == '"') quoted = false;
      } else if (c == '"') quoted = true;
      else if (c == '{' || c == '[') {
        if (++depth > 16) throw std::runtime_error("BytePlus TTS JSON depth limit");
      } else if (c == '}' || c == ']') {
        if (--depth == 0) { consume(); pending.clear(); }
      }
    }
    return true;
  };
  const auto response = transport_.perform(request);
  check();
  if (response.status != 200) throw std::runtime_error("BytePlus TTS HTTP error");
  if (!done || !pending.empty()) throw std::runtime_error("incomplete BytePlus TTS stream");
}
}  // namespace byteturn
