#include "byteturn/websocket_mock_providers.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>

namespace byteturn {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Cancelled = std::function<bool()>;

struct CancelledRequest : std::runtime_error {
  CancelledRequest() : std::runtime_error("mock WebSocket request cancelled") {}
};

WebSocketMockConfig checked(WebSocketMockConfig c) {
  if (!c.port || c.timeout.count() <= 0 || c.timeout > std::chrono::minutes(1) ||
      !c.max_message_bytes || c.max_message_bytes > 1024 * 1024 ||
      !c.max_response_bytes || c.max_response_bytes > 16 * 1024 * 1024 ||
      !c.max_response_messages || c.max_response_messages > 4096)
    throw std::invalid_argument("invalid WebSocket mock bounds");
  return c;
}

// An I/O context is pumped by the provider caller. No adapter worker threads,
// detached operations, or concurrent access to a Beast stream. All asynchronous
// handlers are completed before their stack captures can leave scope.
class Connection {
 public:
  explicit Connection(WebSocketMockConfig c) : config_(c), socket_(io_) {
    socket_.read_message_max(c.max_message_bytes);
  }
  ~Connection() { abort(); }

  void connect(const char* path, Clock::time_point deadline, const Cancelled& stop) {
    net::ip::tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), config_.port);
    run([&](auto done) { socket_.next_layer().async_connect(endpoint, done); }, deadline, stop);
    const auto host = "127.0.0.1:" + std::to_string(config_.port);
    run([&](auto done) { socket_.async_handshake(host, path, done); }, deadline, stop);
    socket_.text(true);
  }

  void send(const Json& message, Clock::time_point deadline, const Cancelled& stop) {
    const auto bytes = message.dump();
    if (bytes.size() > config_.max_message_bytes)
      throw std::runtime_error("mock WebSocket request too large");
    run([&](auto done) {
      socket_.async_write(net::buffer(bytes), [done](auto ec, auto) { done(ec); });
    }, deadline, stop);
  }

  Json receive(std::uint64_t id, Clock::time_point deadline, const Cancelled& stop,
               std::size_t& bytes, std::size_t& messages) {
    if (++messages > config_.max_response_messages)
      throw std::runtime_error("mock WebSocket response message limit");
    beast::flat_buffer buffer(config_.max_message_bytes);
    run([&](auto done) {
      socket_.async_read(buffer, [done](auto ec, auto) { done(ec); });
    }, deadline, stop);
    if (!socket_.got_text() || buffer.size() > config_.max_response_bytes - bytes)
      throw std::runtime_error("mock WebSocket response format/size limit");
    bytes += buffer.size();
    Json result;
    try {
      result = Json::parse(beast::buffers_to_string(buffer.data()),
          [](int depth, Json::parse_event_t, Json&) {
            if (depth > 16) throw std::runtime_error("mock WebSocket JSON nesting limit");
            return true;
          });
    } catch (const Json::exception&) {
      // Parser diagnostics can contain received payload text.
      throw std::runtime_error("invalid mock WebSocket JSON");
    }
    if (!result.is_object() || !result.at("id").is_number_unsigned() ||
        result.at("id").get<std::uint64_t>() != id)
      throw std::runtime_error("mock WebSocket response identity mismatch");
    const auto type = result.at("type").get<std::string>();
    if (type == "error") throw std::runtime_error("mock supplier reported an error");
    return result;
  }

 private:
  void abort() noexcept {
    boost::system::error_code ignored;
    socket_.next_layer().cancel(ignored);
    socket_.next_layer().close(ignored);
  }
  template<class Start>
  void run(Start start, Clock::time_point deadline, const Cancelled& stop) {
    const auto check = [&] {
      if (stop && stop()) throw CancelledRequest();
      if (Clock::now() >= deadline) throw std::runtime_error("mock WebSocket deadline exceeded");
    };
    check();
    bool completed = false;
    boost::system::error_code error;
    io_.restart();
    start([&](boost::system::error_code ec) { error = ec; completed = true; });
    try {
      while (!completed) {
        check();
        io_.run_for(std::chrono::milliseconds(5));
      }
      check();
    } catch (...) {
      abort();
      io_.restart();
      io_.run(); // Drain the cancelled operation before captured locals die.
      throw;
    }
    if (error) throw std::runtime_error("mock WebSocket I/O: " + error.message());
  }
  WebSocketMockConfig config_;
  net::io_context io_;
  ws::stream<net::ip::tcp::socket> socket_;
};

Json audio_json(const AudioFrame& frame, std::size_t limit) {
  if (frame.sample_rate_hz <= 0 || frame.channels <= 0 || frame.channels > 8 ||
      frame.samples.size() % frame.channels || frame.samples.size() > limit / 8)
    throw std::invalid_argument("invalid or oversized mock audio input");
  return {{"samples", frame.samples}, {"sample_rate_hz", frame.sample_rate_hz},
          {"channels", frame.channels}, {"end_of_utterance", frame.end_of_utterance}};
}

AudioFrame parse_audio(const Json& j) {
  AudioFrame frame;
  if (!j.at("sample_rate_hz").is_number_integer() || !j.at("channels").is_number_integer() ||
      j.at("sample_rate_hz") <= 0 || j.at("sample_rate_hz") > 192000 ||
      j.at("channels") <= 0 || j.at("channels") > 8)
    throw std::runtime_error("invalid mock audio metadata");
  frame.sample_rate_hz = j.at("sample_rate_hz").get<int>();
  frame.channels = j.at("channels").get<int>();
  if (frame.sample_rate_hz <= 0 || frame.sample_rate_hz > 192000 ||
      frame.channels <= 0 || frame.channels > 8 || !j.at("samples").is_array())
    throw std::runtime_error("invalid mock audio format");
  for (const auto& sample : j.at("samples")) {
    if (!sample.is_number_integer() || sample < -32768 || sample > 32767)
      throw std::runtime_error("invalid mock PCM sample");
    frame.samples.push_back(sample.get<std::int16_t>());
  }
  if (frame.samples.empty() || frame.samples.size() % frame.channels)
    throw std::runtime_error("invalid mock PCM interleaving");
  return frame;
}

Json history_json(const std::vector<Message>& history, std::size_t limit) {
  if (history.size() > 1024) throw std::invalid_argument("mock history too large");
  Json messages = Json::array();
  std::size_t bytes = 0;
  const auto debit = [&](std::size_t size) {
    if (size > limit - bytes) throw std::invalid_argument("mock history too large");
    bytes += size;
  };
  const auto charge = [&](const std::string& s) { debit(s.size()); };
  for (const auto& message : history) {
    debit(96); // Bound JSON object overhead even for empty strings/arrays.
    charge(message.content); charge(message.name); charge(message.tool_call_id);
    Json calls = Json::array();
    if (message.tool_calls.size() > 128) throw std::invalid_argument("too many mock tool calls");
    for (const auto& call : message.tool_calls) {
      debit(48);
      charge(call.id); charge(call.name); charge(call.arguments);
      calls.push_back({{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    }
    const char* role = nullptr;
    switch (message.role) {
      case Role::System: role = "system"; break;
      case Role::User: role = "user"; break;
      case Role::Assistant: role = "assistant"; break;
      case Role::Tool: role = "tool"; break;
    }
    if (!role) throw std::invalid_argument("invalid mock message role");
    messages.push_back({{"role", role}, {"content", message.content}, {"name", message.name},
                        {"tool_call_id", message.tool_call_id}, {"tool_calls", calls}});
  }
  return messages;
}
}  // namespace

struct WebSocketMockAsr::Impl {
  explicit Impl(WebSocketMockConfig c) : config(checked(c)) {}
  WebSocketMockConfig config;
  std::atomic<std::uint64_t> epoch{0};
  std::uint64_t connection_epoch = 0, request_id = 0;
  std::unique_ptr<Connection> connection;
};
WebSocketMockAsr::WebSocketMockAsr(WebSocketMockConfig c) : impl_(std::make_unique<Impl>(c)) {}
WebSocketMockAsr::~WebSocketMockAsr() = default;
void WebSocketMockAsr::reset() { impl_->epoch.fetch_add(1); }
void WebSocketMockAsr::push(const AudioFrame& frame,
                            const std::function<void(std::string, bool)>& sink) {
  auto& p = *impl_;
  const auto epoch = p.epoch.load();
  const auto stop = [&] { return p.epoch.load() != epoch; };
  const auto deadline = Clock::now() + p.config.timeout;
  try {
    auto audio = audio_json(frame, p.config.max_message_bytes);
    if (!p.connection || p.connection_epoch != epoch) {
      p.connection = std::make_unique<Connection>(p.config);
      p.connection_epoch = epoch;
      p.connection->connect("/asr", deadline, stop);
    }
    if (p.request_id == std::numeric_limits<std::uint64_t>::max())
      throw std::runtime_error("mock request identity exhausted");
    const auto id = ++p.request_id;
    p.connection->send({{"id", id}, {"type", "audio"}, {"audio", audio}}, deadline, stop);
    std::size_t bytes = 0, messages = 0;
    while (true) {
      const auto reply = p.connection->receive(id, deadline, stop, bytes, messages);
      const auto type = reply.at("type").get<std::string>();
      if (type == "done") return;
      if (type != "transcript") throw std::runtime_error("unexpected mock ASR event");
      sink(reply.at("text").get<std::string>(), reply.at("final").get<bool>());
    }
  } catch (const CancelledRequest&) { p.connection.reset(); }
  catch (...) { p.connection.reset(); throw; }
}

WebSocketMockLlm::WebSocketMockLlm(WebSocketMockConfig c) : config_(checked(c)) {}
LlmTurn WebSocketMockLlm::complete(const std::vector<Message>& history) { return complete(history, {}); }
LlmTurn WebSocketMockLlm::complete(const std::vector<Message>& history, const Cancelled& stop) {
  return stream(history, [](const std::string&) { return true; }, stop);
}
LlmTurn WebSocketMockLlm::stream(const std::vector<Message>& history,
                                const TextDeltaSink& sink, const Cancelled& stop) {
  if (stop && stop()) throw CancelledRequest();
  const auto deadline = Clock::now() + config_.timeout;
  const auto request = Json{{"id", 1}, {"type", "generate"},
                            {"messages", history_json(history, config_.max_message_bytes)}};
  Connection connection(config_);
  connection.connect("/llm", deadline, stop);
  connection.send(request, deadline, stop);
  LlmTurn result;
  std::size_t bytes = 0, messages = 0;
  while (true) {
    const auto reply = connection.receive(1, deadline, stop, bytes, messages);
    const auto type = reply.at("type").get<std::string>();
    if (type == "done") {
      if (!reply.at("tool_calls").is_array() || reply.at("tool_calls").size() > 128)
        throw std::runtime_error("invalid mock tool calls");
      for (const auto& call : reply.at("tool_calls"))
        result.tool_calls.push_back({call.at("id").get<std::string>(),
            call.at("name").get<std::string>(), call.at("arguments").get<std::string>()});
      return result;
    }
    if (type != "text_delta") throw std::runtime_error("unexpected mock LLM event");
    auto delta = reply.at("text").get<std::string>();
    if (!delta.empty() && !sink(delta)) throw CancelledRequest();
    result.text += delta;
  }
}

WebSocketMockTts::WebSocketMockTts(WebSocketMockConfig c) : config_(checked(c)) {}
void WebSocketMockTts::begin_utterance() { cancelled_.store(false); }
void WebSocketMockTts::cancel() { cancelled_.store(true); }
void WebSocketMockTts::synthesize(const std::string& text,
                                 const std::function<bool(const AudioFrame&)>& sink) {
  begin_utterance(); synthesize_chunk(text, sink); end_utterance();
}
void WebSocketMockTts::synthesize_chunk(const std::string& text,
                                       const std::function<bool(const AudioFrame&)>& sink) {
  if (text.empty() || cancelled_.load()) return;
  if (text.size() > config_.max_message_bytes) throw std::invalid_argument("mock TTS text too large");
  const auto stop = [&] { return cancelled_.load(); };
  const auto deadline = Clock::now() + config_.timeout;
  try {
    Connection connection(config_);
    connection.connect("/tts", deadline, stop);
    connection.send({{"id", 1}, {"type", "synthesize"}, {"text", text}}, deadline, stop);
    std::size_t bytes = 0, messages = 0;
    while (true) {
      const auto reply = connection.receive(1, deadline, stop, bytes, messages);
      const auto type = reply.at("type").get<std::string>();
      if (type == "done") return;
      if (type != "audio") throw std::runtime_error("unexpected mock TTS event");
      if (!sink(parse_audio(reply.at("audio")))) return;
    }
  } catch (const CancelledRequest&) {} // Matches TtsProvider's cooperative cancel contract.
}
}  // namespace byteturn
