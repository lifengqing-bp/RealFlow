#pragma once

#include "byteturn/observability.h"
#include "byteturn/openai_compatible.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>

namespace byteturn {

struct CurlTransportConfig {
  std::chrono::milliseconds connect_timeout{3000};
  std::chrono::milliseconds request_timeout{30000};
  std::chrono::milliseconds initial_backoff{200};
  std::chrono::milliseconds max_backoff{5000};
  std::size_t max_response_bytes = 8 * 1024 * 1024;
  unsigned max_attempts = 3;
  bool retry_http_errors = true;
  bool https_only = true;
  bool verify_tls = true;
  std::string ca_bundle;
  std::string user_agent = "ByteTurn/0.1";
  MetricsRegistry* metrics = nullptr;
  std::function<void(std::chrono::milliseconds)> sleep;
};

class CurlHttpTransport final : public HttpTransport {
 public:
  explicit CurlHttpTransport(CurlTransportConfig config = {});
  HttpResponse perform(const HttpRequest& request) override;

  static bool retryable_http_status(int status);
  static std::chrono::milliseconds parse_retry_after(
      const std::string& value,
      std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now());

 private:
  CurlTransportConfig config_;
};

}  // namespace byteturn

