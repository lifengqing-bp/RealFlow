#include "byteturn/executor.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace byteturn {

struct SessionExecutor::Work {
  std::string turn_id;
  Task task;
  std::shared_ptr<std::atomic<bool>> cancelled;
  std::promise<std::string> promise;
};

struct SessionExecutor::Session {
  std::deque<std::shared_ptr<Work>> queue;
  std::shared_ptr<Work> running;
  bool scheduled = false;
};

SessionExecutor::SessionExecutor(std::size_t worker_count) {
  if (worker_count == 0) worker_count = std::max(1u, std::thread::hardware_concurrency());
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i)
    workers_.emplace_back([this] { worker_loop(); });
}

SessionExecutor::~SessionExecutor() { shutdown(); }

TurnHandle SessionExecutor::submit(std::string session_id, Task task) {
  auto work = std::make_shared<Work>();
  work->task = std::move(task);
  work->cancelled = std::make_shared<std::atomic<bool>>(false);
  std::future<std::string> result = work->promise.get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw std::runtime_error("session executor is stopped");
    work->turn_id = std::to_string(next_turn_id_++);
    auto& session = sessions_[session_id];
    session.queue.push_back(work);
    if (!session.scheduled && !session.running) {
      session.scheduled = true;
      ready_sessions_.push_back(session_id);
      ready_cv_.notify_one();
    }
  }
  return {work->turn_id, std::move(result)};
}

void SessionExecutor::cancel_session(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) return;
  auto& session = it->second;
  if (session.running) session.running->cancelled->store(true, std::memory_order_release);
  for (auto& work : session.queue) {
    work->cancelled->store(true, std::memory_order_release);
    work->promise.set_exception(
        std::make_exception_ptr(std::runtime_error("turn cancelled")));
  }
  session.queue.clear();
}

void SessionExecutor::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    for (auto& entry : sessions_) {
      if (entry.second.running)
        entry.second.running->cancelled->store(true, std::memory_order_release);
      for (auto& work : entry.second.queue) {
        work->cancelled->store(true, std::memory_order_release);
        work->promise.set_exception(
            std::make_exception_ptr(std::runtime_error("executor stopped")));
      }
      entry.second.queue.clear();
    }
  }
  ready_cv_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
  workers_.clear();
}

void SessionExecutor::worker_loop() {
  while (true) {
    std::string session_id;
    std::shared_ptr<Work> work;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      ready_cv_.wait(lock, [this] { return stopping_ || !ready_sessions_.empty(); });
      if (stopping_ && ready_sessions_.empty()) return;
      session_id = std::move(ready_sessions_.front());
      ready_sessions_.pop_front();
      auto& session = sessions_.at(session_id);
      session.scheduled = false;
      if (session.queue.empty()) continue;
      work = std::move(session.queue.front());
      session.queue.pop_front();
      session.running = work;
    }

    try {
      CancellationToken token(work->cancelled);
      if (token.cancelled()) throw std::runtime_error("turn cancelled");
      work->promise.set_value(work->task(token, work->turn_id));
    } catch (...) {
      work->promise.set_exception(std::current_exception());
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& session = sessions_.at(session_id);
      session.running.reset();
      if (!session.queue.empty() && !stopping_) {
        session.scheduled = true;
        ready_sessions_.push_back(session_id);
        ready_cv_.notify_one();
      } else if (session.queue.empty()) {
        sessions_.erase(session_id);
      }
    }
  }
}

}  // namespace byteturn
