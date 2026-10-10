#pragma once

#include "byteturn/runtime_manager.h"

#include <deque>
#include <mutex>

namespace realflow_app {
using namespace byteturn;

// Supplied by the trusted transport, never decoded from a request body.
// connection_id is server-issued and unique for each authenticated connection.
struct AuthenticatedClient {
  std::string user_id;
  std::string connection_id;
};

enum class ChatCode { Ok, Forbidden, StaleSession, Disconnected, NotRunning,
                      Stopping, AwaitingPlayer, AdmissionRejected, InputRejected, InvalidAudio };
enum class ChatPhase { Idle, Starting, Running, Stopping, Retired };
struct ChatStatus {
  ChatCode code = ChatCode::Ok;
  ChatPhase phase = ChatPhase::Idle;
  SessionHandle handle;
  SessionOutcome startup = SessionOutcome::None;
  SessionOutcome retirement = SessionOutcome::None;
  bool flush_requested = false;
  bool player_stopped = false;
  std::uint64_t dropped_output = 0;
};
struct StartReply {
  ChatStatus status;
  SessionAdmission admission;
};
struct ChatOutput {
  enum class Kind { Audio, Transcript, Observation };
  Kind kind = Kind::Audio;
  SessionHandle handle;
  AudioFrame audio;
  std::string text;
  bool final = false;
  Event event{EventType::Error};
};
struct MediaSinks {
  std::function<void(const AudioFrame&)> audio;
  std::function<void(const std::string&, bool)> transcript;
};
struct ChatConfig {
  std::size_t max_output_messages = 256;
  std::size_t max_payload_bytes = 64 * 1024;
  std::chrono::milliseconds liveness_timeout{30000};
};

// One binding per authenticated connection, owned by the transport. No registry,
// supervisor, worker or provider ownership here. RuntimeManager must outlive it.
// All methods are thread-safe. Factories run on RuntimeManager's startup workers
// and must return session-local dependencies. Do not reenter this binding from
// engine push_audio; media/observation callbacks only enqueue into a separate lock.
class VoiceChatConnection {
 public:
  using Clock = std::chrono::steady_clock;
  using Factory = std::function<SessionResources(const SessionHandle&, MediaSinks)>;
  VoiceChatConnection(RuntimeManager& runtime, AuthenticatedClient client,
                      Factory factory, ChatConfig config = {},
                      Clock::time_point now = Clock::now());
  ~VoiceChatConnection();
  VoiceChatConnection(const VoiceChatConnection&) = delete;
  VoiceChatConnection& operator=(const VoiceChatConnection&) = delete;

  StartReply start(const AuthenticatedClient& client);
  ChatCode stop(const AuthenticatedClient& client, const SessionHandle& handle);
  ChatCode push_audio(const AuthenticatedClient& client, const SessionHandle& handle,
                      AudioFrame frame);
  ChatStatus status(const AuthenticatedClient& client, const SessionHandle& handle);
  // Transport serializes taking/sending packets with stop/flush on its own lane.
  // Packets carry the full incarnation; the player must reject stale packets.
  std::vector<ChatOutput> take_output(const AuthenticatedClient& client,
                                    const SessionHandle& handle);
  ChatCode acknowledge_player_stop(const AuthenticatedClient& client,
                                   const SessionHandle& handle);
  // These are trusted transport hooks. No timer thread is added: call tick from
  // the host loop even when the client is silent. now must be a monotonic clock.
  ChatCode heartbeat(const AuthenticatedClient& client, Clock::time_point now);
  void tick(Clock::time_point now);
  void disconnect();

 private:
  struct Route;
  bool authorized(const AuthenticatedClient&) const;
  ChatCode validate(const AuthenticatedClient&, const SessionHandle&) const;
  void refresh();  // mutex_ held; tickets/find are authoritative, not notifications.
  void close_route();
  void stop_locked();
  ChatStatus snapshot(ChatCode code = ChatCode::Ok) const;

  RuntimeManager& runtime_;
  const AuthenticatedClient client_;
  const Factory factory_;
  const ChatConfig config_;
  const std::string session_id_;
  std::mutex mutex_;
  bool connected_ = true;
  Clock::time_point last_seen_;
  SessionAdmission admission_;
  std::shared_ptr<Route> route_;
  ChatPhase phase_ = ChatPhase::Idle;
  bool flush_requested_ = false, player_stopped_ = false;
};
}  // namespace realflow_app
