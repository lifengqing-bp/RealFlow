#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace byteturn {

class TurnContext;
struct TurnOptions;

class CancellationToken {
 public:
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> cancelled)
      : cancelled_(std::move(cancelled)) {}
  bool cancelled() const { return cancelled_->load(std::memory_order_acquire); }

 private:
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

struct TurnHandle {
  std::string turn_id;
  std::shared_future<std::string> result;
};

struct SessionExecutorConfig {
  std::size_t worker_count = 0;
  std::size_t max_pending_per_session = 4;
  std::size_t max_pending_total = 1024;
};

class ExecutorOverloaded : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class SessionExecutor {
 public:
  using Task =
      std::function<std::string(const CancellationToken&, const std::string& turn_id)>;
  using ContextTask = std::function<std::string(const TurnContext&)>;

  explicit SessionExecutor(std::size_t worker_count = 0);
  explicit SessionExecutor(SessionExecutorConfig config);
  ~SessionExecutor();
  SessionExecutor(const SessionExecutor&) = delete;
  SessionExecutor& operator=(const SessionExecutor&) = delete;

  TurnHandle submit(std::string session_id, Task task);
  TurnHandle submit_turn(std::string session_id, TurnOptions options,
                         ContextTask task);
  void cancel_session(const std::string& session_id);
  void shutdown();

 private:
  struct Work;
  struct Session;
  void worker_loop();

  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::unordered_map<std::string, Session> sessions_;
  std::deque<std::string> ready_sessions_;
  std::vector<std::thread> workers_;
  std::uint64_t next_turn_id_ = 1;
  SessionExecutorConfig config_;
  std::size_t pending_total_ = 0;
  bool stopping_ = false;
};

}  // namespace byteturn
