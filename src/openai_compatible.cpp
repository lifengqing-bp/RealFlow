#include "byteturn/openai_compatible.h"

#include <cctype>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace byteturn {
namespace {

struct Json {
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

  const Json& at(const std::string& key) const {
    const auto* object = std::get_if<Object>(&value);
    if (!object) throw std::runtime_error("expected JSON object");
    const auto it = object->find(key);
    if (it == object->end()) throw std::runtime_error("missing JSON field: " + key);
    return it->second;
  }
  const Json* find(const std::string& key) const {
    const auto* object = std::get_if<Object>(&value);
    if (!object) return nullptr;
    const auto it = object->find(key);
    return it == object->end() ? nullptr : &it->second;
  }
  const Array& array() const {
    const auto* result = std::get_if<Array>(&value);
    if (!result) throw std::runtime_error("expected JSON array");
    return *result;
  }
  std::string string_or_empty() const {
    const auto* result = std::get_if<std::string>(&value);
    return result ? *result : std::string{};
  }
  std::size_t index_or_zero() const {
    const auto* result = std::get_if<double>(&value);
    return result && *result >= 0 ? static_cast<std::size_t>(*result) : 0;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& input) : input_(input) {}
  Json parse() {
    Json result = value();
    space();
    if (position_ != input_.size()) fail("trailing data");
    return result;
  }

 private:
  Json value() {
    space();
    if (position_ == input_.size()) fail("unexpected end");
    const char c = input_[position_];
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') return Json{string()};
    if (c == 't') return literal("true", Json{true});
    if (c == 'f') return literal("false", Json{false});
    if (c == 'n') return literal("null", Json{nullptr});
    return number();
  }

  Json object() {
    ++position_;
    Json::Object result;
    space();
    if (consume('}')) return Json{std::move(result)};
    while (true) {
      space();
      if (position_ == input_.size() || input_[position_] != '"') fail("object key");
      std::string key = string();
      space();
      if (!consume(':')) fail("colon");
      result.emplace(std::move(key), value());
      space();
      if (consume('}')) break;
      if (!consume(',')) fail("comma");
    }
    return Json{std::move(result)};
  }

  Json array() {
    ++position_;
    Json::Array result;
    space();
    if (consume(']')) return Json{std::move(result)};
    while (true) {
      result.push_back(value());
      space();
      if (consume(']')) break;
      if (!consume(',')) fail("comma");
    }
    return Json{std::move(result)};
  }

  std::string string() {
    ++position_;
    std::string result;
    while (position_ < input_.size()) {
      const char c = input_[position_++];
      if (c == '"') return result;
      if (c != '\\') {
        result.push_back(c);
        continue;
      }
      if (position_ == input_.size()) fail("escape");
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': {
          unsigned codepoint = hex4();
          if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u')
              fail("missing low surrogate");
            position_ += 2;
            const unsigned low = hex4();
            if (low < 0xdc00 || low > 0xdfff) fail("invalid low surrogate");
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
          } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            fail("unexpected low surrogate");
          }
          append_utf8(result, codepoint);
          break;
        }
        default: fail("unsupported escape");
      }
    }
    fail("unterminated string");
  }

  Json number() {
    const auto begin = position_;
    while (position_ < input_.size()) {
      const char c = input_[position_];
      if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' ||
            c == '.' || c == 'e' || c == 'E')) break;
      ++position_;
    }
    if (begin == position_) fail("value");
    return Json{std::stod(input_.substr(begin, position_ - begin))};
  }

  unsigned hex4() {
    if (position_ + 4 > input_.size()) fail("short unicode escape");
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = input_[position_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value += static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value += static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value += static_cast<unsigned>(c - 'A' + 10);
      else fail("invalid unicode escape");
    }
    return value;
  }

  static void append_utf8(std::string& output, unsigned codepoint) {
    if (codepoint <= 0x7f) output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
      output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
  }

  Json literal(const char* text, Json result) {
    const std::string expected(text);
    if (input_.compare(position_, expected.size(), expected) != 0) fail("literal");
    position_ += expected.size();
    return result;
  }
  bool consume(char c) {
    if (position_ < input_.size() && input_[position_] == c) {
      ++position_;
      return true;
    }
    return false;
  }
  void space() {
    while (position_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
  }
  [[noreturn]] void fail(const char* reason) const {
    throw std::runtime_error("invalid JSON near byte " + std::to_string(position_) +
                             ": " + reason);
  }

  const std::string& input_;
  std::size_t position_ = 0;
};

std::string escape(const std::string& input) {
  std::string result;
  for (const unsigned char c : input) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (c < 0x20) throw std::invalid_argument("control character in JSON string");
        result.push_back(static_cast<char>(c));
    }
  }
  return result;
}

const char* role_name(Role role) {
  switch (role) {
    case Role::System: return "system";
    case Role::User: return "user";
    case Role::Assistant: return "assistant";
    case Role::Tool: return "tool";
  }
  return "user";
}

std::string message_json(const Message& message) {
  std::string out = "{\"role\":\"" + std::string(role_name(message.role)) + "\"";
  if (message.role == Role::Tool) {
    out += ",\"tool_call_id\":\"" + escape(message.tool_call_id) + "\"";
  }
  out += ",\"content\":\"" + escape(message.content) + "\"";
  if (!message.tool_calls.empty()) {
    out += ",\"tool_calls\":[";
    for (std::size_t i = 0; i < message.tool_calls.size(); ++i) {
      if (i) out += ',';
      const auto& call = message.tool_calls[i];
      out += "{\"id\":\"" + escape(call.id) +
             "\",\"type\":\"function\",\"function\":{\"name\":\"" +
             escape(call.name) + "\",\"arguments\":\"" +
             escape(call.arguments) + "\"}}";
    }
    out += ']';
  }
  return out + '}';
}

class SseParser {
 public:
  using EventSink = std::function<void(const std::string&)>;
  explicit SseParser(EventSink sink) : sink_(std::move(sink)) {}

  bool feed(std::string_view bytes) {
    buffer_.append(bytes.data(), bytes.size());
    while (true) {
      const auto newline = buffer_.find('\n');
      if (newline == std::string::npos) break;
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      process_line(line);
    }
    return true;
  }

  void finish() {
    if (!buffer_.empty()) {
      if (buffer_.back() == '\r') buffer_.pop_back();
      process_line(buffer_);
      buffer_.clear();
    }
    dispatch();
  }

 private:
  void process_line(const std::string& line) {
    if (line.empty()) {
      dispatch();
      return;
    }
    if (line.rfind("data:", 0) != 0) return;
    std::string value = line.substr(5);
    if (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (!data_.empty()) data_ += '\n';
    data_ += value;
  }
  void dispatch() {
    if (data_.empty()) return;
    std::string event;
    event.swap(data_);
    sink_(event);
  }

  EventSink sink_;
  std::string buffer_;
  std::string data_;
};

}  // namespace

OpenAiCompatibleLlm::OpenAiCompatibleLlm(OpenAiCompatibleConfig config,
                                         HttpTransport& transport)
    : config_(std::move(config)), transport_(transport) {
  if (config_.model.empty()) throw std::invalid_argument("model is required");
}

HttpRequest OpenAiCompatibleLlm::make_request(
    const std::vector<Message>& history) const {
  std::string body = "{\"model\":\"" + escape(config_.model) + "\",\"messages\":[";
  for (std::size_t i = 0; i < history.size(); ++i) {
    if (i) body += ',';
    body += message_json(history[i]);
  }
  body += "],\"temperature\":" + std::to_string(config_.temperature);
  if (!config_.tools.empty()) {
    body += ",\"tools\":[";
    for (std::size_t i = 0; i < config_.tools.size(); ++i) {
      if (i) body += ',';
      const auto& tool = config_.tools[i];
      body += "{\"type\":\"function\",\"function\":{\"name\":\"" +
              escape(tool.name) + "\",\"description\":\"" +
              escape(tool.description) + "\",\"parameters\":" +
              tool.parameters_json + "}}";
    }
    body += "]";
  }
  body += '}';

  std::string base = config_.base_url;
  while (!base.empty() && base.back() == '/') base.pop_back();
  HttpRequest request{"POST", base + "/chat/completions",
                      {{"Content-Type", "application/json"}}, std::move(body), {}, {}};
  if (!config_.api_key.empty())
    request.headers["Authorization"] = "Bearer " + config_.api_key;
  return request;
}

LlmTurn OpenAiCompatibleLlm::parse_response(const HttpResponse& response) {
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error("LLM HTTP status " + std::to_string(response.status));
  const Json root = JsonParser(response.body).parse();
  const auto& choices = root.at("choices").array();
  if (choices.empty()) throw std::runtime_error("LLM response has no choices");
  const Json& message = choices.front().at("message");
  LlmTurn result;
  result.text = message.at("content").string_or_empty();
  try {
    for (const auto& item : message.at("tool_calls").array()) {
      const auto& function = item.at("function");
      result.tool_calls.push_back({item.at("id").string_or_empty(),
                                   function.at("name").string_or_empty(),
                                   function.at("arguments").string_or_empty()});
    }
  } catch (const std::runtime_error& e) {
    if (std::string(e.what()).find("missing JSON field: tool_calls") == std::string::npos)
      throw;
  }
  return result;
}

LlmTurn OpenAiCompatibleLlm::complete(const std::vector<Message>& history) {
  return parse_response(transport_.perform(make_request(history)));
}

LlmTurn OpenAiCompatibleLlm::complete(
    const std::vector<Message>& history,
    const std::function<bool()>& cancelled) {
  HttpRequest request = make_request(history);
  request.cancelled = cancelled;
  return parse_response(transport_.perform(request));
}

LlmTurn OpenAiCompatibleLlm::stream(
    const std::vector<Message>& history, const TextDeltaSink& on_text_delta,
    const std::function<bool()>& cancelled) {
  HttpRequest request = make_request(history);
  request.cancelled = cancelled;
  request.body.pop_back();
  request.body += ",\"stream\":true}";
  request.headers["Accept"] = "text/event-stream";

  LlmTurn result;
  bool done = false;
  SseParser parser([&](const std::string& event) {
    // The terminal marker fences every later payload, including tool calls.
    if (done) return;
    if (event == "[DONE]") {
      done = true;
      return;
    }
    const Json root = JsonParser(event).parse();
    const Json* choices_value = root.find("choices");
    if (!choices_value || choices_value->array().empty()) return;
    const Json& choice = choices_value->array().front();
    const Json* delta = choice.find("delta");
    if (!delta) return;
    if (const Json* content = delta->find("content")) {
      const std::string text = content->string_or_empty();
      if (!text.empty()) {
        result.text += text;
        if (!on_text_delta(text))
          throw std::runtime_error("LLM stream consumer stopped");
      }
    }
    if (const Json* calls = delta->find("tool_calls")) {
      for (const auto& item : calls->array()) {
        const Json* index = item.find("index");
        const std::size_t i = index ? index->index_or_zero() : 0;
        if (result.tool_calls.size() <= i) result.tool_calls.resize(i + 1);
        auto& call = result.tool_calls[i];
        if (const Json* id = item.find("id")) call.id += id->string_or_empty();
        if (const Json* function = item.find("function")) {
          if (const Json* name = function->find("name"))
            call.name += name->string_or_empty();
          if (const Json* arguments = function->find("arguments"))
            call.arguments += arguments->string_or_empty();
        }
      }
    }
  });
  request.body_sink = [&](std::string_view bytes) { return parser.feed(bytes); };
  const HttpResponse response = transport_.perform(request);
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error("LLM HTTP status " + std::to_string(response.status));
  parser.finish();
  result.complete = done;
  return result;
}

}  // namespace byteturn
