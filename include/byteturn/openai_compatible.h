#pragma once

#include "byteturn/providers.h"

#include <map>
#include <string>
#include <vector>

namespace byteturn {

struct HttpRequest {
  std::string method;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  std::string body;
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

  HttpRequest make_request(const std::vector<Message>& history) const;
  static LlmTurn parse_response(const HttpResponse& response);

 private:
  OpenAiCompatibleConfig config_;
  HttpTransport& transport_;
};

}  // namespace byteturn

