#include "byteturn/runtime_manager.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {
struct Backend { std::atomic<unsigned> frames{0}; };
class DemoEngine final : public byteturn::ConversationEngine {
 public:
  explicit DemoEngine(Backend& backend) : backend_(backend) {}
  void start(byteturn::ConversationEngineContext) override {}
  bool push_audio(byteturn::AudioFrame) override { ++backend_.frames; return true; }
  void handle_event(const byteturn::Event&) override {}
  void stop() override {}
  byteturn::ConversationCapabilities capabilities() const override { return {}; }
  byteturn::ConversationStateSnapshot state() const override { return {}; }
 private:
  Backend& backend_;  // Kept alive by SessionResources, not the transport.
};
}

int main() {
  // The binary owns the runtime. An RTC transport would retain handles only.
  byteturn::RuntimeManager runtime;
  auto factory = [](const byteturn::SessionHandle&) {
    auto backend = std::make_shared<Backend>();
    byteturn::SessionResources resources;
    resources.dependencies = backend;
    resources.engine = std::make_unique<DemoEngine>(*backend);
    return resources;
  };
  auto first = runtime.add("alice", factory);
  auto second = runtime.add("bob", factory);
  if (!first || !second ||
      first.started.get() != byteturn::SessionOutcome::Started ||
      second.started.get() != byteturn::SessionOutcome::Started)
    throw std::runtime_error("session startup failed");

  runtime.push_audio(first.handle, {{1, 2, 3}, 16000, 1, false});
  runtime.remove(first.handle);  // Returns before the session is destroyed.
  first.retired.get();          // Owning thread, never a media/observer callback.
  runtime.shutdown();          // Closes admission and also retires bob.

  const auto stats = runtime.stats();
  std::cout << "RealFlow runtime: admitted=" << stats.admitted
            << " retired=" << stats.retired << " live=" << stats.sessions << '\n';
  return stats.retired == 2 && stats.sessions == 0 ? 0 : 1;
}
