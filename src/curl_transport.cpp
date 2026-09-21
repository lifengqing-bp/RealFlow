#include "byteturn/curl_transport.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

namespace byteturn {
namespace {

struct TransferState {
  HttpResponse response;
  std::size_t limit = 0;
  bool too_large = false;
  const HttpRequest* request = nullptr;
};

struct EasyHandle {
  EasyHandle() : value(curl_easy_init()) {
    if (!value) throw std::runtime_error("curl easy initialization failed");
  }
  ~EasyHandle() { curl_easy_cleanup(value); }
  EasyHandle(const EasyHandle&) = delete;
  EasyHandle& operator=(const EasyHandle&) = delete;
  CURL* value;
};

CURL* reusable_easy_handle() {
  // SessionExecutor uses stable worker threads. One handle per thread keeps the
  // connection cache hot without sharing an easy handle concurrently.
  thread_local EasyHandle handle;
  curl_easy_reset(handle.value);
  return handle.value;
}

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

std::string lower(std::string value) {
  for (char& c : value) c = static_cast<char>(
      std::tolower(static_cast<unsigned char>(c)));
  return value;
}

size_t write_body(char* data, size_t size, size_t count, void* userdata) {
  auto& state = *static_cast<TransferState*>(userdata);
  const std::size_t bytes = size * count;
  if (bytes > state.limit - std::min(state.limit, state.response.body.size())) {
    state.too_large = true;
    return 0;
  }
  state.response.body.append(data, bytes);
  return bytes;
}

size_t write_header(char* data, size_t size, size_t count, void* userdata) {
  auto& state = *static_cast<TransferState*>(userdata);
  const std::size_t bytes = size * count;
  std::string line(data, bytes);
  const auto colon = line.find(':');
  if (colon != std::string::npos) {
    const std::string name = lower(trim(line.substr(0, colon)));
    const std::string value = trim(line.substr(colon + 1));
    if (!name.empty()) state.response.headers[name] = value;
  }
  return bytes;
}

int progress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  const auto& state = *static_cast<TransferState*>(userdata);
  return state.request->cancelled && state.request->cancelled() ? 1 : 0;
}

bool retryable_curl(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_HTTP2:
      return true;
    default: return false;
  }
}

std::chrono::milliseconds jittered(std::chrono::milliseconds value) {
  thread_local std::mt19937 generator(std::random_device{}());
  std::uniform_real_distribution<double> distribution(0.75, 1.25);
  return std::chrono::milliseconds(
      static_cast<long long>(value.count() * distribution(generator)));
}

void initialize_curl() {
  static std::once_flag once;
  static CURLcode result = CURLE_FAILED_INIT;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) throw std::runtime_error("curl global initialization failed");
}

}  // namespace

CurlHttpTransport::CurlHttpTransport(CurlTransportConfig config)
    : config_(std::move(config)) {
  if (config_.max_attempts == 0) throw std::invalid_argument("max_attempts must be positive");
  if (config_.max_response_bytes == 0)
    throw std::invalid_argument("max_response_bytes must be positive");
  if (config_.connect_timeout.count() <= 0 || config_.request_timeout.count() <= 0)
    throw std::invalid_argument("timeouts must be positive");
  if (!config_.sleep) config_.sleep = [](std::chrono::milliseconds duration) {
    std::this_thread::sleep_for(duration);
  };
  initialize_curl();
}

bool CurlHttpTransport::retryable_http_status(int status) {
  return status == 408 || status == 429 || status == 500 || status == 502 ||
         status == 503 || status == 504;
}

std::chrono::milliseconds CurlHttpTransport::parse_retry_after(
    const std::string& value, std::chrono::system_clock::time_point now) {
  try {
    std::size_t consumed = 0;
    const long long seconds = std::stoll(trim(value), &consumed);
    if (consumed == trim(value).size() && seconds > 0)
      return std::chrono::seconds(seconds);
  } catch (...) {
  }
  const time_t date = curl_getdate(value.c_str(), nullptr);
  if (date < 0) return std::chrono::milliseconds(0);
  const auto target = std::chrono::system_clock::from_time_t(date);
  if (target <= now) return std::chrono::milliseconds(0);
  return std::chrono::duration_cast<std::chrono::milliseconds>(target - now);
}

HttpResponse CurlHttpTransport::perform(const HttpRequest& request) {
  std::chrono::milliseconds backoff = config_.initial_backoff;
  for (unsigned attempt = 1; attempt <= config_.max_attempts; ++attempt) {
    if (request.cancelled && request.cancelled())
      throw std::runtime_error("HTTP request cancelled");

    CURL* curl = reusable_easy_handle();
    curl_slist* headers = nullptr;
    for (const auto& header : request.headers) {
      const std::string line = header.first + ": " + header.second;
      curl_slist* appended = curl_slist_append(headers, line.c_str());
      if (!appended) {
        curl_slist_free_all(headers);
        throw std::runtime_error("failed to allocate HTTP headers");
      }
      headers = appended;
    }
    TransferState state;
    state.limit = config_.max_response_bytes;
    state.request = &request;
    char error[CURL_ERROR_SIZE] = {};

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(config_.connect_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(config_.request_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config_.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config_.verify_tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config_.verify_tls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, config_.https_only ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     config_.https_only ? "https" : "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    if (!config_.ca_bundle.empty())
      curl_easy_setopt(curl, CURLOPT_CAINFO, config_.ca_bundle.c_str());

    const auto started = std::chrono::steady_clock::now();
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    double total_seconds = 0.0;
    double dns_seconds = 0.0;
    double connect_seconds = 0.0;
    double tls_seconds = 0.0;
    double first_byte_seconds = 0.0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_seconds);
    curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &dns_seconds);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_seconds);
    curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &tls_seconds);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &first_byte_seconds);
    state.response.status = static_cast<int>(status);
    state.response.total_time_ms = total_seconds * 1000.0;
    state.response.dns_time_ms = dns_seconds * 1000.0;
    state.response.connect_time_ms = connect_seconds * 1000.0;
    state.response.tls_time_ms = tls_seconds * 1000.0;
    state.response.first_byte_time_ms = first_byte_seconds * 1000.0;
    curl_slist_free_all(headers);

    if (config_.metrics) {
      config_.metrics->increment("byteturn_http_attempts_total");
      config_.metrics->observe(
          "byteturn_http_attempt_duration_ms",
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - started).count());
      config_.metrics->observe("byteturn_http_dns_ms", state.response.dns_time_ms);
      config_.metrics->observe("byteturn_http_connect_ms", state.response.connect_time_ms);
      config_.metrics->observe("byteturn_http_tls_ms", state.response.tls_time_ms);
      config_.metrics->observe("byteturn_http_first_byte_ms",
                               state.response.first_byte_time_ms);
      if (state.response.status > 0) {
        config_.metrics->increment("byteturn_http_responses_total");
        if (state.response.status >= 400)
          config_.metrics->increment("byteturn_http_error_responses_total");
      }
    }
    if (state.too_large) throw std::runtime_error("HTTP response exceeds configured limit");
    if (code == CURLE_ABORTED_BY_CALLBACK)
      throw std::runtime_error("HTTP request cancelled");

    const bool retry = attempt < config_.max_attempts &&
        (retryable_curl(code) ||
         (code == CURLE_OK && config_.retry_http_errors &&
          retryable_http_status(state.response.status)));
    if (!retry) {
      if (code != CURLE_OK) {
        if (config_.metrics) config_.metrics->increment("byteturn_http_errors_total");
        const std::string detail = error[0] ? error : curl_easy_strerror(code);
        throw std::runtime_error("HTTP transport failed: " + detail);
      }
      return state.response;
    }

    if (config_.metrics) config_.metrics->increment("byteturn_http_retries_total");
    auto delay = jittered(backoff);
    const auto retry_after = state.response.headers.find("retry-after");
    if (retry_after != state.response.headers.end())
      delay = std::max(delay, parse_retry_after(retry_after->second));
    delay = std::min(delay, config_.max_backoff);
    config_.sleep(delay);
    backoff = std::min(backoff * 2, config_.max_backoff);
  }
  throw std::runtime_error("HTTP retry loop exhausted");
}

}  // namespace byteturn
