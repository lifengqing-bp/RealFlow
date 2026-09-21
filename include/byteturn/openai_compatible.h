#pragma once

#include "byteturn/providers.h"

#include <map>
#include <functional>
#include <string>
#include <vector>

namespace byteturn {

struct HttpRequest {
  std::string method;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
  std::function<bool()> cancelled;
};

struct HttpResponse {
  int status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
  double total_time_ms = 0.0;
  double dns_time_ms = 0.0;
  double connect_time_ms = 0.0;
  double tls_time_ms = 0.0;
  double first_byte_time_ms = 0.0;
};

class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse perform(const HttpRequest& request) = 0;
};

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_json = R"({"type":"object","properties":{}})";
};

struct OpenAiCompatibleConfig {
  std::string base_url = "https://api.openai.com/v1";
  std::string api_key;
  std::string model;
  std::vector<ToolDefinition> tools;
  double temperature = 0.2;
};

class OpenAiCompatibleLlm final : public LlmProvider {
 public:
  OpenAiCompatibleLlm(OpenAiCompatibleConfig config, HttpTransport& transport);
  LlmTurn complete(const std::vector<Message>& history) override;
  LlmTurn complete(const std::vector<Message>& history,
                   const std::function<bool()>& cancelled) override;

  HttpRequest make_request(const std::vector<Message>& history) const;
  static LlmTurn parse_response(const HttpResponse& response);

 private:
  OpenAiCompatibleConfig config_;
  HttpTransport& transport_;
};

}  // namespace byteturn
