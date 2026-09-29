#include "redis_pvxs_ioc/pv_registry.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sys/socket.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

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
  assert(!registry->publish(rejected, [](std::string& failure) {
    failure = "injected final cutover rejection"; return false;
  }, error));
  assert(client.get("alias").exec()->wait(2.)["value"].as<int>() == 42);
  value["value"] = 43; owner->post(value); observed(43);

  const auto aliases = registry->prepare({{"canonical", binding}, {"new-alias", binding}});
  assert(registry->publish(aliases, {}, error));
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
}
