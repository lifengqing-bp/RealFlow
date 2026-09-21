#include "byteturn/executor.h"
#include "byteturn/turn_context.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace byteturn {

struct SessionExecutor::Work {
  std::string turn_id;
  ContextTask task;
  std::shared_ptr<std::atomic<bool>> cancelled;
  std::shared_ptr<TurnContext> context;
  std::promise<std::string> promise;
};

struct SessionExecutor::Session {
  std::deque<std::shared_ptr<Work>> queue;
  std::shared_ptr<Work> running;
  bool scheduled = false;
};

SessionExecutor::SessionExecutor(std::size_t worker_count)
    : SessionExecutor(SessionExecutorConfig{worker_count, 4, 1024}) {}

SessionExecutor::SessionExecutor(SessionExecutorConfig config)
    : config_(config) {
  if (config_.worker_count == 0)
    config_.worker_count = std::max(1u, std::thread::hardware_concurrency());
  if (config_.max_pending_per_session == 0 || config_.max_pending_total == 0)
    throw std::invalid_argument("executor queue limits must be positive");
  workers_.reserve(config_.worker_count);
  for (std::size_t i = 0; i < config_.worker_count; ++i)
    workers_.emplace_back([this] { worker_loop(); });
}

SessionExecutor::~SessionExecutor() { shutdown(); }

TurnHandle SessionExecutor::submit(std::string session_id, Task task) {
  return submit_turn(
      std::move(session_id), {},
      [task = std::move(task)](const TurnContext& context) {
        if (context.stop_requested())
          throw std::runtime_error(context.expired() ? "turn deadline exceeded"
                                                     : "turn cancelled");
        return task(context.cancellation(), context.turn_id());
      });
}

TurnHandle SessionExecutor::submit_turn(std::string session_id,
                                        TurnOptions options,
                                        ContextTask task) {
  if (session_id.empty()) throw std::invalid_argument("session id is required");
  if (options.timeout.count() <= 0)
    throw std::invalid_argument("turn timeout must be positive");
  auto work = std::make_shared<Work>();
  work->task = std::move(task);
  work->cancelled = std::make_shared<std::atomic<bool>>(false);
  std::shared_future<std::string> result = work->promise.get_future().share();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) throw std::runtime_error("session executor is stopped");
    work->turn_id = options.turn_id.empty() ? std::to_string(next_turn_id_++)
                                             : std::move(options.turn_id);
    const auto existing = sessions_.find(session_id);
    if (existing != sessions_.end() &&
        existing->second.queue.size() >= config_.max_pending_per_session)
      throw ExecutorOverloaded("session pending-turn limit reached");
    if (pending_total_ >= config_.max_pending_total)
      throw ExecutorOverloaded("executor pending-turn limit reached");
    auto& session = sessions_[session_id];
    if (options.trace_id.empty()) options.trace_id = session_id + ":" + work->turn_id;
    work->context = std::make_shared<TurnContext>(
        session_id, work->turn_id, std::move(options.trace_id),
        CancellationToken(work->cancelled),
        std::chrono::steady_clock::now() + options.timeout);
    session.queue.push_back(work);
    ++pending_total_;
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
  pending_total_ -= session.queue.size();
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
      pending_total_ -= entry.second.queue.size();
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
      --pending_total_;
      session.running = work;
    }

    try {
      if (work->context->expired())
        work->cancelled->store(true, std::memory_order_release);
      work->context->transition(TurnState::Running);
      work->promise.set_value(work->task(*work->context));
      work->context->transition(TurnState::Completed);
    } catch (...) {
      work->context->transition(
          work->context->expired() ? TurnState::TimedOut
          : work->context->cancelled() ? TurnState::Cancelled
                                        : TurnState::Failed);
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
