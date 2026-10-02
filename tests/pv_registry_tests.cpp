#include "redis_pvxs_ioc/pv_registry.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sys/socket.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

namespace {
class Gate {
public:
  void pause() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    assert(changed_.wait_for(lock, 5s, [&] { return released_; }));
  }
  void waitEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    assert(changed_.wait_for(lock, 3s, [&] { return entered_; }));
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }
private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false, released_ = false;
};

struct ChannelState {
  bool attached = false;
  unsigned closedAfterAttach = 0;
};

// A controllable ChannelControl lets retirement drain its channel list while
// onCreate is paused before/during SharedPV::attach, without blocking the PVXS
// event loop. Keep real SharedPV attachment and onClose cleanup in the test.
class TestChannel final : public pvxs::server::ChannelControl {
public:
  explicit TestChannel(std::shared_ptr<ChannelState> state)
      : ChannelControl("racing-alias", std::make_shared<pvxs::server::ClientCredentials>(), None),
        state_(std::move(state)) {}
  void onOp(std::function<void(std::unique_ptr<pvxs::server::ConnectOp>&&)>&& fn) override {
    onOp_ = std::move(fn);
  }
  void onRPC(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    onRPC_ = std::move(fn);
  }
  void onSubscribe(std::function<void(std::unique_ptr<pvxs::server::MonitorSetupOp>&&)>&& fn) override {
    onSubscribe_ = std::move(fn);
  }
  void onClose(std::function<void(const std::string&)>&& fn) override {
    state_->attached = true;
    onClose_ = std::move(fn);
  }
  void close() override {
    if (state_->attached) ++state_->closedAfterAttach;
    state_->attached = false;
    auto callback = std::move(onClose_);
    onOp_ = {}; onRPC_ = {}; onSubscribe_ = {};
    if (callback) callback("retired");
  }
private:
  void _updateInfo(const std::shared_ptr<const pvxs::server::ReportInfo>&) override {}
  std::shared_ptr<ChannelState> state_;
  std::function<void(std::unique_ptr<pvxs::server::ConnectOp>&&)> onOp_;
  std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)> onRPC_;
  std::function<void(std::unique_ptr<pvxs::server::MonitorSetupOp>&&)> onSubscribe_;
  std::function<void(const std::string&)> onClose_;
};

void retiringCreate(bool duringAttach) {
  PVRegistry registry;
  auto owner = std::make_shared<pvxs::server::SharedPV>(pvxs::server::SharedPV::buildReadonly());
  owner->open(pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create());
  const PVBinding binding{*owner, owner, {}};
  Gate gate;
  PVRegistry::Wrappers wrappers;
  if (duringAttach) owner->onFirstConnect([&](pvxs::server::SharedPV&) { gate.pause(); });
  else wrappers.emplace("racing-alias", [&](PVRegistry::Channel channel) {
    gate.pause(); return channel;
  });
  const auto initial = registry.prepare({{"canonical", binding}, {"racing-alias", binding}}, wrappers);
  std::string error;
  assert(registry.publish(initial, {}, error));
  registry.finish(initial);
  const auto retiring = registry.prepare({{"canonical", binding}});
  const auto state = std::make_shared<ChannelState>();
  auto creation = std::async(std::launch::async, [&] {
    PVRegistry::Channel channel = std::make_unique<TestChannel>(state);
    registry.onCreate(std::move(channel));
    assert(!channel);
  });
  gate.waitEntered();
  auto publication = std::async(std::launch::async, [&] {
    const auto result = registry.publish(retiring, {}, error);
    if (result) registry.finish(retiring);
    return result;
  });
  const bool retiredWhileCreating = publication.wait_for(1s) == std::future_status::ready;
  gate.release();
  assert(publication.get());
  creation.get();
  assert(retiredWhileCreating);
  assert(!state->attached && state->closedAfterAttach > 0);
  assert(owner->isOpen());
  assert(registry.onList().names->count("canonical"));
  assert(!registry.onList().names->count("racing-alias"));
  registry.clear();
}
}

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  auto registry = std::make_shared<PVRegistry>();
  auto owner = std::make_shared<pvxs::server::SharedPV>(pvxs::server::SharedPV::buildReadonly());
  auto value = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  value["value"] = 42;
  owner->open(value);
  PVBinding binding{*owner, owner, {}};
  auto prepared = registry->prepare({{"canonical", binding}, {"alias", binding}});
  std::string error;
  assert(registry->publish(prepared, {}, error));
  registry->finish(prepared);
  server.addSource("registry", registry).start();
  auto client = server.clientConfig().build();
  assert(client.get("alias").exec()->wait(2.)["value"].as<int>() == 42);
  const auto publishWhileServing = [&](const std::shared_ptr<PVRegistry::Prepared>& next,
                                       bool accept, int expected) {
    Gate gate;
    auto publication = std::async(std::launch::async, [&] {
      return registry->publish(next, [&](std::string& failure) {
        gate.pause();
        if (!accept) failure = "injected final cutover rejection";
        return accept;
      }, error);
    });
    gate.waitEntered();
    auto listing = std::async(std::launch::async, [&] { return registry->onList(); });
    auto preparation = std::async(std::launch::async, [&] { return registry->prepare(registry->bindings()); });
    // A new context must search and create a channel; no cached channel can
    // hide either callback waiting behind the slow final commit action.
    auto fresh = server.clientConfig().build();
    bool available = false;
    try { available = fresh.get("alias").exec()->wait(1.)["value"].as<int>() == expected; }
    catch (const std::exception&) {}
    const bool listed = listing.wait_for(1s) == std::future_status::ready;
    const bool preparedWithoutWaiting = preparation.wait_for(1s) == std::future_status::ready;
    gate.release();
    const auto published = publication.get();
    const auto names = listing.get().names;
    const auto overlapping = preparation.get();
    fresh.close();
    assert(available && listed && preparedWithoutWaiting);
    assert(names->count("canonical") && names->count("alias") && !names->count("new-alias"));
    assert(published == accept);
    if (accept) {
      bool invoked = false;
      assert(!registry->publish(overlapping, [&](std::string&) { invoked = true; return true; }, error));
      assert(!invoked);
    }
    return published;
  };
  std::mutex mutex;
  std::condition_variable changed;
  auto monitor = client.monitor("canonical").maskConnected(true).maskDisconnected(false)
      .event([&](pvxs::client::Subscription&) { changed.notify_all(); }).exec();
  const auto observed = [&](int expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    for (;;) {
      // An unexpected Disconnect is a regression, not a retry condition.
      if (const auto update = monitor->pop(); update && update["value"].as<int>() == expected) return;
      std::unique_lock<std::mutex> lock(mutex);
      assert(std::chrono::steady_clock::now() < deadline);
      changed.wait_for(lock, 5ms);
    }
  };
  observed(42);
  auto replacement = std::make_shared<pvxs::server::SharedPV>(pvxs::server::SharedPV::buildReadonly());
  auto changedValue = value.clone();
  changedValue["value"] = 999;
  replacement->open(changedValue);
  const auto rejected = registry->prepare({{"canonical", {*replacement, replacement, {}}}});
  assert(!publishWhileServing(rejected, false, 42));
  assert(client.get("alias").exec()->wait(2.)["value"].as<int>() == 42);
  value["value"] = 43; owner->post(value); observed(43);

  const auto aliases = registry->prepare({{"canonical", binding}, {"new-alias", binding}});
  assert(publishWhileServing(aliases, true, 43));
  registry->finish(aliases);
  assert(owner->isOpen());
  value["value"] = 44; owner->post(value); observed(44);
  assert(client.get("new-alias").exec()->wait(2.)["value"].as<int>() == 44);
  assert(!registry->onList().names->count("alias"));

  const auto stale = registry->prepare(registry->bindings());
  const auto newer = registry->prepare(registry->bindings());
  assert(registry->publish(newer, {}, error));
  bool invoked = false;
  assert(!registry->publish(stale, [&](std::string&) { invoked = true; return true; }, error));
  assert(!invoked);
  registry->finish(newer);
  monitor.reset();
  registry->clear();
  client.close();
  server.stop();
  retiringCreate(false);
  retiringCreate(true);
}
