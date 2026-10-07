#pragma once

#include "byteturn/event.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace realflow_test {
using namespace std::chrono_literals;

inline void check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

template <class Exception = std::runtime_error, class F>
std::string expect_throw(F&& call) {
  try { call(); }
  catch (const Exception& error) { return error.what(); }
  throw std::runtime_error("expected exception was not thrown");
}

// A signal is sticky: readiness never depends on a scheduling delay.
class Signal {
 public:
  void set() {
    std::lock_guard<std::mutex> lock(mutex_);
    set_ = true;
    cv_.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    check(cv_.wait_for(lock, 5s, [&] { return set_; }), "signal timed out");
  }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool set_ = false;
};

// Declare after futures/owners that need unblocking on an assertion failure.
class Finally {
 public:
  explicit Finally(std::function<void()> action) : action_(std::move(action)) {}
  ~Finally() { action_(); }
  Finally(const Finally&) = delete;
  Finally& operator=(const Finally&) = delete;
 private:
  std::function<void()> action_;
};

template <class T> T await(std::future<T>& future) {
  check(future.wait_for(5s) == std::future_status::ready, "future timed out");
  return future.get();
}
template <class T> T await(const std::shared_future<T>& future) {
  check(future.wait_for(5s) == std::future_status::ready, "future timed out");
  return future.get();
}

inline std::size_t count(const std::vector<byteturn::Event>& events,
                         byteturn::EventType type) {
  std::size_t result = 0;
  for (const auto& event : events) if (event.type == type) ++result;
  return result;
}
inline std::size_t index(const std::vector<byteturn::Event>& events,
                         byteturn::EventType type) {
  for (std::size_t i = 0; i < events.size(); ++i) if (events[i].type == type) return i;
  throw std::runtime_error("required event is missing");
}

// Assertions belong on the test thread, not inside EventBus handlers: handler
// exceptions are intentionally contained by the runtime.
class EventCapture {
 public:
  void push(const byteturn::Event& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
    cv_.notify_all();
  }
  void wait(byteturn::EventType type, std::size_t n = 1) {
    std::unique_lock<std::mutex> lock(mutex_);
    check(cv_.wait_for(lock, 5s, [&] { return count(events_, type) >= n; }),
          "event did not arrive");
  }
  std::vector<byteturn::Event> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }
 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<byteturn::Event> events_;
};

using Test = std::pair<const char*, std::function<void()>>;
inline int run(int argc, char** argv, const std::vector<Test>& tests) {
  std::string selected;
  if (argc == 2 && std::string(argv[1]) == "--list") {
    for (const auto& test : tests) std::cout << test.first << '\n';
    return 0;
  }
  if (argc == 3 && std::string(argv[1]) == "--case") selected = argv[2];
  else if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " [--list | --case NAME]\n";
    return 2;
  }
  std::size_t executed = 0, failed = 0;
  for (const auto& test : tests) {
    if (!selected.empty() && selected != test.first) continue;
    ++executed;
    try {
      test.second();
      std::cout << "PASS " << test.first << std::endl;
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << std::endl;
    } catch (...) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": non-standard exception\n";
    }
  }
  if (!executed) { std::cerr << "no matching test: " << selected << '\n'; return 2; }
  std::cout << executed - failed << '/' << executed << " cases passed\n";
  return failed ? 1 : 0;
}
}  // namespace realflow_test
