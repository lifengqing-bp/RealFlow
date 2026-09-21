#pragma once

#include "byteturn/executor.h"

#include <atomic>
#include <chrono>
#include <string>

namespace byteturn {

enum class TurnState {
  Queued,
  Running,
  ModelStreaming,
  ToolExecuting,
  Synthesizing,
  Completed,
  Cancelled,
  TimedOut,
  Failed
};

struct TurnOptions {
  std::string turn_id;
  std::string trace_id;
  std::chrono::milliseconds timeout{15000};
};

class TurnContext {
 public:
  TurnContext(std::string session_id, std::string turn_id, std::string trace_id,
              CancellationToken cancellation,
              std::chrono::steady_clock::time_point deadline);

  const std::string& session_id() const { return session_id_; }
  const std::string& turn_id() const { return turn_id_; }
  const std::string& trace_id() const { return trace_id_; }
  const CancellationToken& cancellation() const { return cancellation_; }
  bool cancelled() const { return cancellation_.cancelled(); }
  bool expired() const { return std::chrono::steady_clock::now() >= deadline_; }
  bool stop_requested() const { return cancelled() || expired(); }
  std::chrono::milliseconds remaining() const;
  TurnState state() const { return state_.load(std::memory_order_acquire); }
  void transition(TurnState state) const {
    state_.store(state, std::memory_order_release);
  }

 private:
  std::string session_id_;
  std::string turn_id_;
  std::string trace_id_;
  CancellationToken cancellation_;
  std::chrono::steady_clock::time_point deadline_;
  mutable std::atomic<TurnState> state_{TurnState::Queued};
};

}  // namespace byteturn
