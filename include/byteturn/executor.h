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
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace byteturn {

class CancellationToken {
 public:
  bool cancelled() const { return cancelled_->load(std::memory_order_acquire); }

 private:
  friend class SessionExecutor;
  explicit CancellationToken(std::shared_ptr<std::atomic<bool>> cancelled)
      : cancelled_(std::move(cancelled)) {}
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

struct TurnHandle {
  std::string turn_id;
  std::future<std::string> result;
};

class SessionExecutor {
 public:
  using Task =
      std::function<std::string(const CancellationToken&, const std::string& turn_id)>;

  explicit SessionExecutor(std::size_t worker_count = 0);
  ~SessionExecutor();
  SessionExecutor(const SessionExecutor&) = delete;
  SessionExecutor& operator=(const SessionExecutor&) = delete;

  TurnHandle submit(std::string session_id, Task task);
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
  bool stopping_ = false;
};

}  // namespace byteturn
