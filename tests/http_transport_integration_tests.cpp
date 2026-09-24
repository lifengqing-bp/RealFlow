#include "test_support.h"
#include "byteturn/agent.h"
#include "byteturn/curl_transport.h"

#include <atomic>

namespace {
using namespace byteturn;
using namespace realflow_test;
std::string base;
CurlTransportConfig local_config() {
  CurlTransportConfig c;
  c.https_only = false;  // Only this loopback fixture opts into HTTP.
  c.max_attempts = 3; c.connect_timeout = 1s; c.request_timeout = 2s;
  c.initial_backoff = 0ms; c.max_backoff = 10ms;
  c.sleep = [](std::chrono::milliseconds) {};
  return c;
}
HttpRequest request(const std::string& path) {
  return {"POST", base + path, {{"Content-Type", "text/plain"}}, "fixture request", {}, {}};
}
int requests(const std::string& path) {
  auto c = local_config(); c.max_attempts = 1;
  CurlHttpTransport t(c);
  return std::stoi(t.perform(request("/counts?path=" + path)).body);
}
void real_http_round_trip_and_metrics() {
  MetricsRegistry metrics; auto c = local_config(); c.metrics = &metrics;
  CurlHttpTransport transport(c); auto r = request("/echo");
  r.body = u8"你好\n\"escaped\""; r.headers["Authorization"] = "Bearer fixture-only";
  const auto response = transport.perform(r);
  check(response.status == 200 && response.body == r.body &&
        response.headers.at("x-fixture") == "echo" && response.headers.at("x-method") == "POST" &&
        response.headers.at("x-auth") == "Bearer fixture-only", "real request/response encoding and headers");
  check(requests("/echo") == 1 && metrics.counter("byteturn_http_attempts_total") == 1 &&
        metrics.counter("byteturn_http_responses_total") == 1, "one successful network attempt");
  check(metrics.histogram("byteturn_provider_first_packet_ms").count == 1 &&
        metrics.histogram("byteturn_provider_request_duration_ms").count == 1 &&
        metrics.histogram("byteturn_http_first_byte_ms").count == 1,
        "network metric endpoints present without assuming exact timing");
}
void retry_count_and_retry_after_cap() {
  MetricsRegistry metrics; auto c = local_config(); c.metrics = &metrics;
  std::vector<std::chrono::milliseconds> delays;
  c.sleep = [&](auto delay) { delays.push_back(delay); };
  CurlHttpTransport t(c); const auto r = t.perform(request("/retry"));
  check(r.status == 200 && r.body == "ready" && requests("/retry") == 3, "two failures then one success");
  check(delays == std::vector<std::chrono::milliseconds>{10ms, 10ms}, "Retry-After bounded by configured cap");
  check(metrics.counter("byteturn_http_attempts_total") == 3 &&
        metrics.counter("byteturn_http_retries_total") == 2 &&
        metrics.counter("byteturn_http_error_responses_total") == 2 &&
        metrics.histogram("byteturn_provider_request_duration_ms").count == 1, "attempts versus logical request");
}
void retry_budget_and_non_retryable_status() {
  auto c = local_config(); c.max_attempts = 2; CurlHttpTransport t(c);
  check(t.perform(request("/always-503")).status == 503 && requests("/always-503") == 2, "retry budget bounded");
  check(t.perform(request("/bad-request")).status == 400 && requests("/bad-request") == 1, "400 not retried");
}
void streaming_status_body_is_not_replayed() {
  CurlHttpTransport t(local_config()); auto r = request("/stream-503"); std::string body;
  r.body_sink = [&](std::string_view part) { body += part; return true; };
  const auto result = t.perform(r);
  check(result.status == 503 && result.body.empty() && body == "retryable body" &&
        requests("/stream-503") == 1, "no retry after streaming body delivery");
}
void response_limits_apply_to_buffered_and_streaming() {
  for (bool streaming : {false, true}) {
    const std::string path = streaming ? "/stream-large" : "/large";
    auto c = local_config(); c.max_response_bytes = 64; CurlHttpTransport t(c); auto r = request(path);
    std::size_t bytes = 0;
    if (streaming) r.body_sink = [&](std::string_view b) { bytes += b.size(); return true; };
    auto error = expect_throw([&] { t.perform(r); });
    check(error.find("configured limit") != std::string::npos && bytes <= 64 && requests(path) == 1,
          "bounded payload with no retry on overflow");
  }
}
void sink_exception_and_rejection_stop_transfer() {
  CurlHttpTransport t(local_config()); auto r = request("/sink-throws");
  r.body_sink = [](std::string_view) -> bool { throw std::domain_error("fixture consumer failure"); };
  check(expect_throw<std::domain_error>([&] { t.perform(r); }) == "fixture consumer failure" &&
        requests("/sink-throws") == 1, "C callback preserves exception and never retries it");
  r = request("/sink-stops"); r.body_sink = [](std::string_view) { return false; };
  expect_throw([&] { t.perform(r); });
  check(requests("/sink-stops") == 1, "sink rejection stops transfer without replay");
}
void truncated_stream_is_not_success() {
  CurlHttpTransport t(local_config()); auto r = request("/truncated"); std::string body;
  r.body_sink = [&](std::string_view b) { body += b; return true; };
  expect_throw([&] { t.perform(r); });
  check(body == "partial-body" && requests("/truncated") == 1, "partial bytes neither replayed nor accepted as complete");
}
void cancellation_before_io_and_during_retry() {
  auto c = local_config(); MetricsRegistry m; c.metrics = &m; bool cancelled = true;
  CurlHttpTransport t(c); auto r = request("/never-requested"); r.cancelled = [&] { return cancelled; };
  expect_throw([&] { t.perform(r); });
  check(requests("/never-requested") == 0 && m.counter("byteturn_http_attempts_total") == 0, "pre-cancel does no network I/O");
  cancelled = false; c.sleep = [&](auto) { cancelled = true; }; CurlHttpTransport retrying(c);
  r.url = base + "/cancel-backoff"; expect_throw([&] { retrying.perform(r); });
  check(requests("/cancel-backoff") == 1 && m.counter("byteturn_http_attempts_total") == 1, "cancellation fences next retry");
}
void timeout_and_redirect_policy() {
  auto c = local_config(); c.max_attempts = 1; c.request_timeout = 100ms; CurlHttpTransport t(c);
  expect_throw([&] { t.perform(request("/timeout")); });
  CurlHttpTransport control(local_config()); control.perform(request("/release-timeout"));
  check(requests("/timeout") == 1, "timed-out attempt bounded");
  check(control.perform(request("/redirect")).status == 302 &&
        requests("/unexpected-redirect-target") == 0, "redirect must not forward credentials automatically");
}
void secure_default_rejects_plain_http() {
  CurlTransportConfig c; c.max_attempts = 1; CurlHttpTransport t(c);
  expect_throw([&] { t.perform(request("/must-use-https")); });
  check(requests("/must-use-https") == 0, "HTTPS-only default preserved");
}
void malformed_sse_fails_over_real_transport() {
  CurlHttpTransport t(local_config()); OpenAiCompatibleConfig c; c.base_url = base + "/bad"; c.model = "fixture";
  OpenAiCompatibleLlm model(c, t); int callbacks = 0;
  expect_throw([&] { model.stream({}, [&](const std::string&) { ++callbacks; return true; }, {}); });
  check(callbacks == 0 && requests("/bad/chat/completions") == 1, "parser failure crosses curl callback safely");
}
void agent_tool_loop_over_real_http_sse() {
  MetricsRegistry metrics; auto tc = local_config(); tc.metrics = &metrics; CurlHttpTransport transport(tc);
  OpenAiCompatibleConfig c; c.base_url = base + "/v1"; c.model = "fixture";
  c.tools = {{"double", "Double a value", R"({"type":"object","properties":{"value":{"type":"integer"}}})"}};
  OpenAiCompatibleLlm model(c, transport); ToolRegistry tools; int effects = 0;
  tools.add("double", [&](const std::string& arguments) {
    check(arguments == R"({"value":21})", "tool fragments decode over real wire"); ++effects; return "42";
  });
  EventBus bus; EventCapture captured; bus.subscribe([&](const Event& e) { captured.push(e); });
  RuntimeObserver observer(bus, metrics); Agent agent(model, tools, 4, &bus);
  std::string text; const auto response = agent.run_streaming("calculate", "http-session", "turn",
      [&](const std::string& part) { text += part; return true; });
  check(response == u8"答案是42。" && text == response && effects == 1 && agent.history().size() == 5,
        "HTTP/SSE -> agent -> tool -> HTTP/SSE integration");
  const auto events = captured.snapshot();
  for (auto type : {EventType::ToolStarted, EventType::ToolCompleted, EventType::TurnCompleted})
    check(count(events, type) == 1, "logical endpoints emitted exactly once");
  check(count(events, EventType::ModelStarted) == 2 && count(events, EventType::ModelCompleted) == 2 &&
        index(events, EventType::ToolCompleted) < index(events, EventType::TurnCompleted), "causal tool/model sequence");
  check(metrics.counter("byteturn_http_attempts_total") == 2 &&
        metrics.histogram("byteturn_tool_duration_ms").count == 1 && observer.pending_spans() == 0,
        "transport and logical-operation metrics correlate without dangling spans");
}
void terminal_sse_cannot_dispatch_a_late_tool() {
  CurlHttpTransport transport(local_config()); OpenAiCompatibleConfig c; c.base_url = base + "/v1"; c.model = "after-done";
  OpenAiCompatibleLlm model(c, transport); ToolRegistry tools; int effects = 0;
  tools.add("effect", [&](const std::string&) { ++effects; return "unexpected"; });
  Agent agent(model, tools, 2);
  check(agent.run("finish") == "Final answer!" && effects == 0, "late payload cannot cause a tool side effect");
}
}  // namespace
int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "run via tests/http_fixture.py\n"; return 2; }
  base = argv[1];
  return run(argc - 1, argv + 1, {
    {"real_http_round_trip_and_metrics", real_http_round_trip_and_metrics},
    {"retry_count_and_retry_after_cap", retry_count_and_retry_after_cap},
    {"retry_budget_and_non_retryable_status", retry_budget_and_non_retryable_status},
    {"streaming_status_body_is_not_replayed", streaming_status_body_is_not_replayed},
    {"response_limits_apply_to_buffered_and_streaming", response_limits_apply_to_buffered_and_streaming},
    {"sink_exception_and_rejection_stop_transfer", sink_exception_and_rejection_stop_transfer},
    {"truncated_stream_is_not_success", truncated_stream_is_not_success},
    {"cancellation_before_io_and_during_retry", cancellation_before_io_and_during_retry},
    {"timeout_and_redirect_policy", timeout_and_redirect_policy},
    {"secure_default_rejects_plain_http", secure_default_rejects_plain_http},
    {"malformed_sse_fails_over_real_transport", malformed_sse_fails_over_real_transport},
    {"agent_tool_loop_over_real_http_sse", agent_tool_loop_over_real_http_sse},
    {"terminal_sse_cannot_dispatch_a_late_tool", terminal_sse_cannot_dispatch_a_late_tool}
  });
}
