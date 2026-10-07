#include "test_support.h"
#include "byteturn/agent.h"
#include "byteturn/curl_transport.h"
#include "byteturn/incremental_tts.h"
#include "byteturn/openai_compatible.h"
#include "byteturn/sentence_segmenter.h"

#include <atomic>
#include <cstdint>
#include <sstream>

namespace {
using namespace byteturn;
using namespace realflow_test;

class ScriptedHttp final : public HttpTransport {
 public:
  HttpResponse response;
  HttpRequest request;
  std::vector<std::string> chunks;
  int calls = 0;
  HttpResponse perform(const HttpRequest& value) override {
    ++calls;
    request = value;
    if (value.body_sink) for (const auto& chunk : chunks)
      if (!value.body_sink(chunk)) throw std::runtime_error("consumer stopped");
    return response;
  }
};
OpenAiCompatibleConfig config() {
  OpenAiCompatibleConfig c;
  c.base_url = "https://example.invalid/v1///";
  c.model = "fixture-model";
  return c;
}
std::string delta(const std::string& text) {
  return "data: {\"choices\":[{\"delta\":{\"content\":\"" + text + "\"}}]}\n\n";
}
LlmTurn stream(ScriptedHttp& http, std::string* observed = nullptr) {
  OpenAiCompatibleLlm model(config(), http);
  return model.stream({{Role::User, "hello", {}, {}, {}}},
      [&](const std::string& text) { if (observed) *observed += text; return true; }, {});
}
HttpResponse json_response(const std::string& content) {
  HttpResponse r; r.status = 200;
  r.body = "{\"choices\":[{\"message\":{\"content\":" + content + "}}]}";
  return r;
}
void request_contract() {
  ScriptedHttp http;
  auto c = config(); c.api_key = "fixture-only-not-a-real-key";
  c.tools = {{"lookup", "Look up a value", R"({"type":"object"})"}};
  OpenAiCompatibleLlm model(c, http);
  const auto r = model.make_request({
      {Role::System, "a\"b\\c\n\t", {}, {}, {}},
      {Role::Assistant, "", {}, {{"call1", "lookup", R"({"x":1})"}}, {}},
      {Role::Tool, "ok", "lookup", {}, "call1"}});
  check(r.method == "POST" && r.url == "https://example.invalid/v1/chat/completions", "endpoint normalization");
  check(r.headers.at("Authorization") == "Bearer fixture-only-not-a-real-key", "authorization");
  check(r.body.find("a\\\"b\\\\c\\n\\t") != std::string::npos &&
        r.body.find("\"tool_call_id\":\"call1\"") != std::string::npos &&
        r.body.find("\"tools\":[") != std::string::npos, "history and tools encoded");
  OpenAiCompatibleLlm anonymous(config(), http);
  check(!anonymous.make_request({}).headers.count("Authorization"), "empty key is not sent");
}
void missing_model_rejected() {
  ScriptedHttp http;
  expect_throw<std::invalid_argument>([&] { OpenAiCompatibleLlm m({}, http); });
  check(http.calls == 0, "invalid config performs no request");
}
void response_unicode_and_null_content() {
  check(OpenAiCompatibleLlm::parse_response(json_response(R"("\u4f60\u597d\ud83d\ude80")")).text == u8"你好🚀", "unicode decoding");
  check(OpenAiCompatibleLlm::parse_response(json_response("null")).text.empty(), "null text supported");
}
void response_invalid_json_rejected() {
  for (const std::string payload : {"{", "{}", "{\"choices\":[]}",
       "{\"choices\":42}", "{\"choices\":[{\"message\":{}}]}"}) {
    HttpResponse r; r.status = 200; r.body = payload;
    expect_throw([&] { OpenAiCompatibleLlm::parse_response(r); });
  }
  auto r = json_response("\"ok\""); r.body += " trailing";
  expect_throw([&] { OpenAiCompatibleLlm::parse_response(r); });
}
void invalid_surrogates_rejected() {
  for (const std::string value : {R"("\ud800")", R"("\udc00")", R"("\ud800\u0041")", R"("\u00xx")"})
    expect_throw([&] { OpenAiCompatibleLlm::parse_response(json_response(value)); });
}
void http_error_payload_not_exposed() {
  HttpResponse r; r.status = 401; r.body = "private-response-and-token";
  const auto message = expect_throw([&] { OpenAiCompatibleLlm::parse_response(r); });
  check(message.find("401") != std::string::npos && message.find(r.body) == std::string::npos, "status without secret payload");
}
void sse_every_split_boundary() {
  // Includes cuts inside CRLF, field names, JSON, and multibyte UTF-8.
  const std::string wire = ": heartbeat\r\nid: ignored\r\nevent: message\r\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"你好🚀\"}}]}\r\n\r\n"
      "data: {\"choices\":[],\"usage\":{\"total_tokens\":3}}\r\n\r\n"
      "data: [DONE]\r\n\r\n";
  for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
    ScriptedHttp http; http.response.status = 200;
    http.chunks = {wire.substr(0, cut), wire.substr(cut)};
    std::string observed; const auto result = stream(http, &observed);
    check(result.complete && result.text == u8"你好🚀" && observed == result.text,
          "SSE fragmentation at byte " + std::to_string(cut));
    check(http.request.headers.at("Accept") == "text/event-stream" &&
          http.request.body.find("\"stream\":true") != std::string::npos, "stream requested");
  }
}
void sse_one_byte_and_multiline() {
  const std::string wire = "data: {\"choices\":\n"
      "data: [{\"delta\":{\"content\":\"hello\"}}]}\n\n"
      "data: [DONE]";  // EOF without trailing delimiter.
  ScriptedHttp http; http.response.status = 200;
  for (char c : wire) http.chunks.emplace_back(1, c);
  const auto result = stream(http);
  check(result.complete && result.text == "hello", "multiline data and EOF dispatch");
}
void sse_tool_calls_interleave() {
  ScriptedHttp http; http.response.status = 200;
  http.chunks = {
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"b\",\"function\":{\"name\":\"second\",\"arguments\":\"{\"}},{\"index\":0,\"id\":\"a\",\"function\":{\"name\":\"first\",\"arguments\":\"{\"}}]}}]}\n\n",
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}},{\"index\":1,\"function\":{\"arguments\":\"}\"}}]}}]}\n\n",
      "data: [DONE]\n\n"};
  const auto r = stream(http);
  check(r.complete && r.tool_calls.size() == 2 && r.tool_calls[0].id == "a" &&
        r.tool_calls[1].name == "second" && r.tool_calls[0].arguments == "{}" &&
        r.tool_calls[1].arguments == "{}", "tool fragments correlated by index");
}
void sse_incomplete_is_not_success() {
  ScriptedHttp http; http.response.status = 200; http.chunks = {delta("partial")};
  const auto r = stream(http);
  check(!r.complete && r.text == "partial", "missing DONE is incomplete");
}
void sse_terminal_ignores_late_payload() {
  ScriptedHttp http; http.response.status = 200;
  http.chunks = {delta("final") + "data: [DONE]\n\n" + delta("stale") +
                 "data: this-is-not-json\n\n"};
  std::string observed; const auto r = stream(http, &observed);
  check(r.complete && r.text == "final" && observed == "final", "DONE fences later payloads");
}
void sse_failure_and_consumer_stop() {
  ScriptedHttp http; http.response.status = 200; http.chunks = {"data: {broken}\n\n"};
  expect_throw([&] { stream(http); });
  http.chunks = {delta("a"), delta("b")};
  OpenAiCompatibleLlm model(config(), http); int calls = 0;
  expect_throw([&] { model.stream({}, [&](const std::string&) { ++calls; return false; }, {}); });
  check(calls == 1, "consumer stop prevents the next delta");
}
void cancellation_predicate_is_forwarded() {
  ScriptedHttp http; http.response = json_response("\"ok\"");
  OpenAiCompatibleLlm model(config(), http); bool cancelled = false;
  model.complete({}, [&] { return cancelled; });
  check(http.request.cancelled && !http.request.cancelled(), "predicate installed");
  cancelled = true; check(http.request.cancelled(), "predicate not captured as a stale value");
}

bool valid_utf8(const std::string& value) {
  for (std::size_t i = 0; i < value.size();) {
    const auto c = static_cast<unsigned char>(value[i]);
    const std::size_t n = c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf ? 2 :
        (c >= 0xe0 && c <= 0xef ? 3 : (c >= 0xf0 && c <= 0xf4 ? 4 : 0)));
    if (!n || i + n > value.size()) return false;
    for (std::size_t j = 1; j < n; ++j)
      if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
    i += n;
  }
  return true;
}
void segmentation_limits_and_reset() {
  for (auto c : {SentenceSegmenterConfig{0, 1, 2}, {2, 1, 2}, {1, 3, 2}})
    expect_throw<std::invalid_argument>([&] { SentenceSegmenter s(c); });
  SentenceSegmenter s;
  check(s.push("unfinished").empty(), "buffer incomplete sentence"); s.reset();
  check(s.flush().empty(), "reset discards old context");
  check(s.push(" \t\n").empty() && s.flush().empty(), "no empty chunks");
}
void segmentation_decimal_and_flush() {
  SentenceSegmenter s({1, 50, 100});
  auto chunks = s.push("Value 3.14. Next");
  check(chunks == std::vector<std::string>{"Value 3.14."}, "decimal is not a sentence boundary");
  check(s.flush() == std::vector<std::string>{"Next"} && s.flush().empty(), "tail flushed exactly once");
}
void segmentation_utf8_hard_limit() {
  SentenceSegmenter s({1, 2, 3});
  const std::string text = u8"你好世界🚀再见";
  auto chunks = s.push(text); auto tail = s.flush();
  chunks.insert(chunks.end(), tail.begin(), tail.end());
  std::string joined;
  for (const auto& chunk : chunks) { check(valid_utf8(chunk), "hard limit split a codepoint"); joined += chunk; }
  check(chunks.size() > 1 && joined == text, "UTF-8 content conserved across chunks");
}
void segmentation_partial_utf8() {
  SentenceSegmenter s({1, 2, 2});
  const std::string text = u8"你🚀好世界";
  std::string joined;
  for (char byte : text) for (const auto& chunk : s.push(std::string(1, byte))) {
    check(valid_utf8(chunk), "incomplete UTF-8 must be buffered"); joined += chunk;
  }
  for (const auto& chunk : s.flush()) { check(valid_utf8(chunk), "valid tail"); joined += chunk; }
  check(joined == text, "fragmented UTF-8 conserved");
  check(s.push(std::string(1, static_cast<char>(0xe4))).empty(), "retain incomplete leading byte");
  expect_throw<std::invalid_argument>([&] { s.flush(); });
}

class TestSpeech final : public TtsProvider {
 public:
  std::function<void(const std::string&, const std::function<bool(const AudioFrame&)>&)> action;
  std::atomic<int> begins{0}, ends{0}, cancels{0};
  void begin_utterance() override { ++begins; }
  void end_utterance() override { ++ends; }
  void cancel() override { ++cancels; }
  void synthesize(const std::string& s, const std::function<bool(const AudioFrame&)>& cb) override {
    if (action) action(s, cb); else cb({{1}, 16000, 1, false});
  }
};
void tts_finish_fences_push() {
  TestSpeech speech; int audio = 0;
  IncrementalTtsPipeline p(speech, [&](const AudioFrame&) { ++audio; return true; });
  check(p.push("one"), "chunk accepted"); p.finish(); p.finish();
  check(!p.push("lost-after-finish"), "finished pipeline must reject new chunks");
  check(audio == 1 && speech.begins == 1 && speech.ends == 1 && speech.cancels == 0,
        "normal completion has one utterance lifecycle");
}
void tts_exception_surfaces_from_finish() {
  TestSpeech speech;
  speech.action = [](const std::string&, const auto&) { throw std::domain_error("fixture synthesis failure"); };
  IncrementalTtsPipeline p(speech, [](const AudioFrame&) { return true; });
  p.push("one");
  check(expect_throw<std::domain_error>([&] { p.finish(); }) == "fixture synthesis failure", "original error preserved");
  check(!p.push("two") && speech.ends == 0, "failed speech has no successful end");
}
void tts_sink_exception_surfaces() {
  TestSpeech speech;
  IncrementalTtsPipeline p(speech, [](const AudioFrame&) -> bool { throw std::domain_error("fixture sink failure"); });
  p.push("one");
  check(expect_throw<std::domain_error>([&] { p.finish(); }) == "fixture sink failure", "sink error preserved");
}
void tts_cancel_unblocks_backpressure() {
  TestSpeech speech; Signal entered, release, producer_entered;
  speech.action = [&](const std::string&, const auto&) { entered.set(); release.wait(); };
  IncrementalTtsPipeline p(speech, [](const AudioFrame&) { return true; }, {}, 1);
  p.push("active"); entered.wait(); p.push("queued");
  auto push = std::async(std::launch::async, [&] { producer_entered.set(); return p.push("blocked"); });
  Finally cleanup([&] { p.cancel(); release.set(); });
  producer_entered.wait();
  check(push.wait_for(20ms) == std::future_status::timeout, "bounded queue prevents unbounded admission");
  p.cancel(); check(!await(push), "cancellation releases blocked producer with rejection");
  release.set(); p.finish();
  check(speech.cancels == 1 && speech.ends == 0, "cancel is not successful completion");
}
void tts_external_cancel_fences_audio() {
  TestSpeech speech; std::atomic<bool> stop{false}; bool second_accepted = true; int n = 0;
  speech.action = [&](const std::string&, const auto& cb) {
    cb({{1}, 16000, 1, false}); stop = true;
    second_accepted = cb({{2}, 16000, 1, false});
  };
  IncrementalTtsPipeline p(speech, [&](const AudioFrame&) { ++n; return true; }, [&] { return stop.load(); });
  p.push("one"); p.finish();
  check(n == 1 && !second_accepted && speech.ends == 0, "external cancellation suppresses stale audio");
}
void tts_invalid_capacity() {
  TestSpeech speech;
  expect_throw<std::invalid_argument>([&] { IncrementalTtsPipeline p(speech, [](const AudioFrame&) { return true; }, {}, 0); });
  check(speech.begins == 0, "invalid config starts no worker");
}
void tool_registration_and_errors() {
  ToolRegistry tools; int calls = 0;
  check(tools.add("lookup", [&](const std::string& s) { ++calls; return s; }), "first tool registration");
  check(!tools.add("lookup", [](const std::string&) { return "wrong"; }), "duplicate must not replace handler");
  check(tools.invoke("lookup", "value") == "value" && calls == 1, "original handler retained");
  check(tools.invoke("missing", "").find("unknown tool") != std::string::npos, "unknown tool is explicit");
  tools.add("broken", [](const std::string&) -> std::string { throw std::runtime_error("broken"); });
  check(tools.invoke("broken", "").find("tool failed") != std::string::npos, "ordinary tool error returned to agent");
}
void tool_cancellation_before_and_after_side_effect() {
  ToolRegistry tools; bool stop = true; int calls = 0;
  tools.add("effect", [&](const std::string&) { ++calls; stop = true; return "done"; });
  expect_throw([&] { tools.invoke("effect", "", [&] { return stop; }); });
  check(calls == 0, "pre-cancel prevents invocation"); stop = false;
  expect_throw([&] { tools.invoke("effect", "", [&] { return stop; }); });
  check(calls == 1, "cancellation does not pretend to undo an existing effect");
}
void curl_config_and_retry_policy() {
  for (int field = 0; field < 4; ++field) {
    CurlTransportConfig c;
    if (field == 0) c.max_attempts = 0;
    if (field == 1) c.max_response_bytes = 0;
    if (field == 2) c.connect_timeout = 0ms;
    if (field == 3) c.request_timeout = 0ms;
    expect_throw<std::invalid_argument>([&] { CurlHttpTransport t(c); });
  }
  for (int code : {408, 429, 500, 502, 503, 504}) check(CurlHttpTransport::retryable_http_status(code), "retryable status");
  for (int code : {200, 301, 400, 401, 403, 404, 501}) check(!CurlHttpTransport::retryable_http_status(code), "non-retryable status");
  const auto now = std::chrono::system_clock::from_time_t(1445412480);
  check(CurlHttpTransport::parse_retry_after(" 3 ", now) == 3s, "numeric Retry-After");
  check(CurlHttpTransport::parse_retry_after("Wed, 21 Oct 2015 07:28:10 GMT", now) == 10s, "HTTP-date Retry-After");
  check(CurlHttpTransport::parse_retry_after("not-a-date", now) == 0ms, "invalid Retry-After");
  check(CurlHttpTransport::parse_retry_after("Wed, 21 Oct 2015 07:27:00 GMT", now) == 0ms, "past date");
}
}  // namespace

int main(int argc, char** argv) {
  return run(argc, argv, {
    {"request_contract", request_contract}, {"missing_model_rejected", missing_model_rejected},
    {"response_unicode_and_null_content", response_unicode_and_null_content},
    {"response_invalid_json_rejected", response_invalid_json_rejected},
    {"invalid_surrogates_rejected", invalid_surrogates_rejected},
    {"http_error_payload_not_exposed", http_error_payload_not_exposed},
    {"sse_every_split_boundary", sse_every_split_boundary},
    {"sse_one_byte_and_multiline", sse_one_byte_and_multiline},
    {"sse_tool_calls_interleave", sse_tool_calls_interleave},
    {"sse_incomplete_is_not_success", sse_incomplete_is_not_success},
    {"sse_terminal_ignores_late_payload", sse_terminal_ignores_late_payload},
    {"sse_failure_and_consumer_stop", sse_failure_and_consumer_stop},
    {"cancellation_predicate_is_forwarded", cancellation_predicate_is_forwarded},
    {"segmentation_limits_and_reset", segmentation_limits_and_reset},
    {"segmentation_decimal_and_flush", segmentation_decimal_and_flush},
    {"segmentation_utf8_hard_limit", segmentation_utf8_hard_limit},
    {"segmentation_partial_utf8", segmentation_partial_utf8},
    {"tts_finish_fences_push", tts_finish_fences_push},
    {"tts_exception_surfaces_from_finish", tts_exception_surfaces_from_finish},
    {"tts_sink_exception_surfaces", tts_sink_exception_surfaces},
    {"tts_cancel_unblocks_backpressure", tts_cancel_unblocks_backpressure},
    {"tts_external_cancel_fences_audio", tts_external_cancel_fences_audio},
    {"tts_invalid_capacity", tts_invalid_capacity},
    {"tool_registration_and_errors", tool_registration_and_errors},
    {"tool_cancellation_before_and_after_side_effect", tool_cancellation_before_and_after_side_effect},
    {"curl_config_and_retry_policy", curl_config_and_retry_policy}
  });
}
