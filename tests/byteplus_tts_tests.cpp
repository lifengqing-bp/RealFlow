#include "byteturn/byteplus_tts.h"

#include <iostream>
#include <stdexcept>

using namespace byteturn;
namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("test assertion failed"); }
template<class F> void fails(F f) {
  bool failed = false;
  try { f(); } catch (const std::exception&) { failed = true; }
  require(failed);
}
struct Transport : HttpTransport {
  std::function<HttpResponse(const HttpRequest&)> action;
  HttpResponse perform(const HttpRequest& r) override { return action(r); }
};
HttpResponse response(int status) { HttpResponse r; r.status = status; return r; }
const std::string audio = R"({"code":0,"data":"AQD//wCA/38="})";
const std::string done = R"({"code":20000000,"data":null})";
BytePlusTtsConfig config() {
  BytePlusTtsConfig c;
  c.api_key = "test-key"; c.speaker = "test-speaker";
  return c;
}
}  // namespace

int main() {
  try {
    Transport transport;
    BytePlusTts provider(config(), transport);
    std::vector<std::int16_t> samples;
    const auto sink = [&](const AudioFrame& frame) {
      require(frame.sample_rate_hz == 24000 && frame.channels == 1);
      samples.insert(samples.end(), frame.samples.begin(), frame.samples.end());
      return true;
    };
    // Every possible two-chunk split, then byte-at-a-time delivery.
    const std::string stream = audio + "\r\n" + done + "\n";
    for (std::size_t split = 0; split <= stream.size(); ++split) {
      transport.action = [&](const HttpRequest& r) {
        require(r.method == "POST");
        require(r.url == "https://voice.ap-southeast-1.bytepluses.com/api/v3/tts/unidirectional");
        require(r.headers.at("X-Api-Key") == "test-key");
        require(r.headers.at("X-Api-Resource-Id") == "seed-tts-2.0");
        require(r.headers.at("X-Api-App-Key") == "aGjiRDfUWi");
        require(r.body.find("test-key") == std::string::npos);
        require(r.body.find("\"format\":\"pcm\"") != std::string::npos);
        require(r.body_sink(std::string_view(stream).substr(0, split)));
        require(r.body_sink(std::string_view(stream).substr(split)));
        return response(200);
      };
      samples.clear();
      provider.synthesize("hello \"世界\"", sink);
      require(samples == std::vector<std::int16_t>({1, -1, -32768, 32767}));
    }
    transport.action = [&](const HttpRequest& r) {
      for (const char& c : stream) r.body_sink(std::string_view(&c, 1));
      return response(200);
    };
    samples.clear(); provider.synthesize("hello", sink); require(samples.size() == 4);
    // An odd PCM byte carries across JSON records, but not across requests.
    transport.action = [&](const HttpRequest& r) {
      r.body_sink(R"({"code":0,"data":"AQ=="}{"code":0,"data":"AA=="})" + done);
      return response(200);
    };
    samples.clear(); provider.synthesize("hello", sink); require(samples == std::vector<std::int16_t>{1});
    const std::vector<std::string> malformed = {
      "", audio, "{", "[]", "garbage", R"({"code":"0"})", R"({"code":0.0})",
      R"({"code":45000000,"message":"secret"})", R"({"code":0,"data":42})",
      R"({"code":0,"data":"!!!!"})", R"({"code":0,"data":"AQ="})",
      R"({"code":0,"data":"AR=="})", R"({"code":0,"data":"AA=A"})",
      R"({"code":0,"data":"AA==AAAA"})", R"({"code":0,"data":"AQ=="})" + done,
      done + audio, done + done, R"({"code":20000000,"data":"AQAA"})",
      R"({"code":0,"data":"","extra":)" + std::string(17, '[')
    };
    for (const auto& bytes : malformed) {
      transport.action = [&](const HttpRequest& r) { r.body_sink(bytes); return response(200); };
      fails([&] { provider.synthesize("hello", sink); });
    }
    transport.action = [&](const HttpRequest& r) { r.body_sink(done); return response(401); };
    fails([&] { provider.synthesize("hello", sink); });
    // Consumer rejection/cancellation stop before a second audio callback.
    transport.action = [&](const HttpRequest& r) {
      r.body_sink(audio + audio + done); return response(200);
    };
    int calls = 0;
    fails([&] { provider.synthesize("hello", [&](const AudioFrame&) { ++calls; return false; }); });
    require(calls == 1);
    calls = 0;
    fails([&] { provider.synthesize("hello", [&](const AudioFrame&) {
      ++calls; provider.cancel(); return true;
    }); });
    require(calls == 1);
    transport.action = [&](const HttpRequest&) -> HttpResponse {
      throw std::logic_error("cancelled chunk must not reach transport");
    };
    try { provider.synthesize_chunk("hello", sink); require(false); }
    catch (const std::runtime_error& e) { require(std::string(e.what()) == "BytePlus TTS cancelled"); }
    transport.action = [&](const HttpRequest& r) {
      provider.cancel(); require(r.cancelled()); r.body_sink(audio); return response(200);
    };
    calls = 0;
    fails([&] { provider.synthesize("hello", [&](const AudioFrame&) { ++calls; return true; }); });
    require(calls == 0);
    transport.action = [&](const HttpRequest& r) { r.body_sink(audio + done); return response(200); };
    provider.begin_utterance(); provider.synthesize_chunk("recovered", sink);
    for (bool total : {false, true}) {
      auto c = config();
      if (total) c.max_response_bytes = 8; else c.max_message_bytes = 8;
      BytePlusTts bounded(c, transport);
      fails([&] { bounded.synthesize("hello", sink); });
    }
    auto c = config(); c.max_text_bytes = 2;
    BytePlusTts small(c, transport); fails([&] { small.synthesize("hello", sink); });
    c = config(); c.api_key = "key\r\nInjected: value";
    fails([&] { BytePlusTts invalid(c, transport); });
    c = config(); c.sample_rate_hz = 12345;
    fails([&] { BytePlusTts invalid(c, transport); });
    fails([&] { provider.synthesize("", sink); });
    fails([&] { provider.synthesize("hello", {}); });
    std::cout << "BytePlus TTS protocol, bounds and cancellation tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
