#include "voice_chat_adapter.h"

#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace realflow_app {
namespace {
bool same(const SessionHandle& a, const SessionHandle& b) {
  return a.runtime_id == b.runtime_id && a.id == b.id && a.generation == b.generation;
}
bool ready(const std::shared_future<SessionOutcome>& ticket) {
  return ticket.valid() && ticket.wait_for(std::chrono::milliseconds(0)) ==
                               std::future_status::ready;
}
std::string next_id() {
  static std::atomic<std::uint64_t> next{1};
  auto value = next.load();
  do {
    if (value == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("connection identity exhausted");
  } while (!next.compare_exchange_weak(value, value + 1));
  return "voice-" + std::to_string(value);
}

// Subscribe before engine startup so observations use the canonical timeline
// identity/sequence. The callback owns only a weak route, never the connection.
class RoutedEngine final : public ConversationEngine {
 public:
  RoutedEngine(std::unique_ptr<ConversationEngine> engine, EventTimeline::Handler sink)
      : engine_(std::move(engine)), sink_(std::move(sink)) {
    if (!engine_) throw std::invalid_argument("voice chat engine is required");
  }
  void start(ConversationEngineContext context) override {
    context.timeline->subscribe(sink_);
    engine_->start(std::move(context));
  }
  bool push_audio(AudioFrame frame) override { return engine_->push_audio(std::move(frame)); }
  bool cancel_response() override { return engine_->cancel_response(); }
  void handle_event(const Event& event) override { engine_->handle_event(event); }
  void stop() override { engine_->stop(); }
  ConversationCapabilities capabilities() const override { return engine_->capabilities(); }
  ConversationStateSnapshot state() const override { return engine_->state(); }
 private:
  std::unique_ptr<ConversationEngine> engine_;
  EventTimeline::Handler sink_;
};
}  // namespace

struct VoiceChatConnection::Route {
  explicit Route(ChatConfig limits) : config(limits) {}
  const ChatConfig config;
  std::mutex mutex;
  bool open = true, overflow = false;
  std::uint64_t dropped = 0;
  std::deque<ChatOutput> queue;

  // No external calls or waits under this lock. Check payload sizes before copy.
  template <class Fill> void append(std::size_t bytes, Fill fill) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!open) return;
    if (bytes > config.max_payload_bytes || queue.size() >= config.max_output_messages) {
      ++dropped;
      overflow = true;
      open = false;
      queue.clear();
      return;
    }
    try {
      ChatOutput output;
      fill(output);
      queue.push_back(std::move(output));
    } catch (...) {
      ++dropped;
      overflow = true;
      open = false;
      queue.clear();
    }
  }
};

VoiceChatConnection::VoiceChatConnection(RuntimeManager& runtime,
    AuthenticatedClient client, Factory factory, ChatConfig config, Clock::time_point now)
    : runtime_(runtime), client_(std::move(client)), factory_(std::move(factory)),
      config_(config), session_id_(next_id()), last_seen_(now) {
  if (client_.user_id.empty() || client_.connection_id.empty() ||
      client_.user_id.size() > 256 || client_.connection_id.size() > 256 || !factory_ ||
      !config_.max_output_messages || !config_.max_payload_bytes ||
      config_.liveness_timeout.count() <= 0)
    throw std::invalid_argument("invalid authenticated voice chat binding");
}
VoiceChatConnection::~VoiceChatConnection() { disconnect(); }

bool VoiceChatConnection::authorized(const AuthenticatedClient& client) const {
  return client.user_id == client_.user_id && client.connection_id == client_.connection_id;
}
ChatCode VoiceChatConnection::validate(const AuthenticatedClient& client,
                                     const SessionHandle& handle) const {
  if (!authorized(client)) return ChatCode::Forbidden;
  if (!connected_) return ChatCode::Disconnected;
  if (!admission_ || !same(handle, admission_.handle)) return ChatCode::StaleSession;
  return ChatCode::Ok;
}
void VoiceChatConnection::close_route() {
  if (!route_) return;
  std::lock_guard<std::mutex> lock(route_->mutex);
  route_->open = false;
  route_->queue.clear();
  flush_requested_ = true;
}
void VoiceChatConnection::stop_locked() {
  if (!admission_) return;
  close_route();
  if (phase_ != ChatPhase::Retired) {
    phase_ = ChatPhase::Stopping;
    runtime_.remove(admission_.handle);
  }
}
void VoiceChatConnection::refresh() {
  if (!admission_) return;
  bool overflow;
  {
    std::lock_guard<std::mutex> lock(route_->mutex);
    overflow = route_->overflow;
  }
  if (overflow && phase_ != ChatPhase::Stopping && phase_ != ChatPhase::Retired) {
    close_route();
    phase_ = ChatPhase::Stopping;
    runtime_.fail(admission_.handle);
  }
  if (ready(admission_.retired)) {
    close_route();
    phase_ = ChatPhase::Retired;
    return;
  }
  const auto info = runtime_.find(admission_.handle);
  if (!info || info->phase == SessionPhase::StopRequested ||
      info->phase == SessionPhase::Retiring ||
      (ready(admission_.started) && admission_.started.get() != SessionOutcome::Started)) {
    close_route();
    phase_ = ChatPhase::Stopping;
  } else if (phase_ == ChatPhase::Starting && ready(admission_.started)) {
    phase_ = ChatPhase::Running;
  }
}
ChatStatus VoiceChatConnection::snapshot(ChatCode code) const {
  ChatStatus result;
  result.code = code;
  if (code == ChatCode::Forbidden || code == ChatCode::StaleSession ||
      code == ChatCode::Disconnected) return result;
  result.phase = phase_;
  result.handle = admission_.handle;
  if (ready(admission_.started)) result.startup = admission_.started.get();
  if (ready(admission_.retired)) result.retirement = admission_.retired.get();
  result.flush_requested = flush_requested_;
  result.player_stopped = player_stopped_;
  if (route_) {
    std::lock_guard<std::mutex> lock(route_->mutex);
    result.dropped_output = route_->dropped;
  }
  return result;
}

StartReply VoiceChatConnection::start(const AuthenticatedClient& client) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!authorized(client)) return {snapshot(ChatCode::Forbidden), {}};
  if (!connected_) return {snapshot(ChatCode::Disconnected), {}};
  refresh();
  if (phase_ == ChatPhase::Starting || phase_ == ChatPhase::Running)
    return {snapshot(), admission_};
  if (phase_ == ChatPhase::Stopping) return {snapshot(ChatCode::Stopping), admission_};
  if (phase_ == ChatPhase::Retired && !player_stopped_)
    return {snapshot(ChatCode::AwaitingPlayer), admission_};

  auto route = std::make_shared<Route>(config_);
  std::weak_ptr<Route> weak = route;
  auto factory = factory_; // Factory must not capture the connection's lifetime.
  auto accepted = runtime_.add(session_id_, [weak, factory](const SessionHandle& handle) {
    MediaSinks sinks;
    sinks.audio = [weak, handle](const AudioFrame& frame) {
      if (auto r = weak.lock()) r->append(frame.samples.size() * sizeof(std::int16_t),
        [&](ChatOutput& out) { out.kind = ChatOutput::Kind::Audio; out.handle = handle;
                              out.audio = frame; });
    };
    sinks.transcript = [weak, handle](const std::string& text, bool final) {
      if (auto r = weak.lock()) r->append(text.size(), [&](ChatOutput& out) {
        out.kind = ChatOutput::Kind::Transcript; out.handle = handle;
        out.text = text; out.final = final;
      });
    };
    auto resources = factory(handle, std::move(sinks));
    resources.engine = std::make_unique<RoutedEngine>(std::move(resources.engine),
      [weak, handle](const Event& event) {
        if (auto r = weak.lock()) {
          const auto bytes = event.session_id.size() + event.turn_id.size() +
              event.trace_id.size() + event.name.size() + event.data.size();
          r->append(bytes, [&](ChatOutput& out) {
            out.kind = ChatOutput::Kind::Observation; out.handle = handle; out.event = event;
          });
        }
      });
    return resources;
  });
  if (!accepted) return {snapshot(ChatCode::AdmissionRejected), accepted};
  admission_ = std::move(accepted);
  route_ = std::move(route);
  phase_ = ChatPhase::Starting;
  flush_requested_ = player_stopped_ = false;
  return {snapshot(), admission_};
}
ChatCode VoiceChatConnection::stop(const AuthenticatedClient& client,
                                  const SessionHandle& handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto code = validate(client, handle);
  if (code != ChatCode::Ok) return code;
  stop_locked();
  refresh();
  return ChatCode::Ok;
}
ChatCode VoiceChatConnection::push_audio(const AuthenticatedClient& client,
                                        const SessionHandle& handle, AudioFrame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto code = validate(client, handle);
  if (code != ChatCode::Ok) return code;
  refresh();
  if (phase_ != ChatPhase::Running) return ChatCode::NotRunning;
  if (frame.samples.size() > config_.max_payload_bytes / sizeof(std::int16_t) ||
      frame.sample_rate_hz <= 0 || frame.channels <= 0 ||
      frame.samples.size() % static_cast<std::size_t>(frame.channels) != 0)
    return ChatCode::InvalidAudio;
  const bool accepted = runtime_.push_audio(handle, std::move(frame));
  refresh();
  return accepted ? ChatCode::Ok :
      phase_ == ChatPhase::Running ? ChatCode::InputRejected : ChatCode::NotRunning;
}
ChatStatus VoiceChatConnection::status(const AuthenticatedClient& client,
                                       const SessionHandle& handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto code = validate(client, handle);
  if (code == ChatCode::Ok) refresh();
  return snapshot(code);
}
std::vector<ChatOutput> VoiceChatConnection::take_output(
    const AuthenticatedClient& client, const SessionHandle& handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (validate(client, handle) != ChatCode::Ok) return {};
  refresh();
  if (phase_ != ChatPhase::Running) return {};
  std::lock_guard<std::mutex> media(route_->mutex);
  std::vector<ChatOutput> result;
  result.reserve(route_->queue.size());
  while (!route_->queue.empty()) {
    result.push_back(std::move(route_->queue.front()));
    route_->queue.pop_front();
  }
  return result;
}
ChatCode VoiceChatConnection::acknowledge_player_stop(
    const AuthenticatedClient& client, const SessionHandle& handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto code = validate(client, handle);
  if (code != ChatCode::Ok) return code;
  refresh();
  if (!flush_requested_) return ChatCode::NotRunning;
  player_stopped_ = true;
  return ChatCode::Ok;
}
ChatCode VoiceChatConnection::heartbeat(const AuthenticatedClient& client, Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!authorized(client)) return ChatCode::Forbidden;
  if (now - last_seen_ >= config_.liveness_timeout) {
    connected_ = false;
    stop_locked();
  }
  if (!connected_) return ChatCode::Disconnected;
  if (now > last_seen_) last_seen_ = now;
  return ChatCode::Ok;
}
void VoiceChatConnection::tick(Clock::time_point now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (now - last_seen_ >= config_.liveness_timeout) {
    connected_ = false;
    stop_locked();
  }
  refresh();
}
void VoiceChatConnection::disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  connected_ = false;
  stop_locked();
}
}  // namespace realflow_app
