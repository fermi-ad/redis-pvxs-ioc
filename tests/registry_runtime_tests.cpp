#include "redis_pvxs_ioc/pv_registry.h"
#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/operation_queue.h"
#include "RedisAdapter.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <future>
#include <set>
#include <sstream>
#include <thread>
#include <sys/socket.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

namespace {
template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(2ms);
  }
}

std::set<std::string> clientsExecuting(swr::Redis& redis, const std::string& command) {
  std::set<std::string> result;
  std::istringstream clients(redis.command<std::string>("CLIENT", "LIST"));
  std::string line;
  while (std::getline(clients, line)) {
    if (line.find(" cmd=" + command + " ") != std::string::npos)
      result.insert(line.substr(0, line.find(' ')));
  }
  return result;
}
}

int main() {
  // run-with-redis.py owns this private server. Pausing only its writes leaves
  // CLIENT LIST/UNPAUSE available to observe and release a real blocked XADD.
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  RA_Options options;
  options.cxn.port = std::stoi(port);
  auto reader = std::make_shared<RedisAdapter>("registry-runtime", options);
  RedisAdapter producer("registry-runtime", options);
  auto writeOptions = options;
  writeOptions.cxn.timeout = 5000;
  auto writer = std::make_shared<RedisAdapter>("registry-runtime", writeOptions);
  RedisBackendRegistry backends{{"default", reader}, {"writer", writer}};
  swr::ConnectionOptions adminOptions;
  adminOptions.port = options.cxn.port;
  adminOptions.socket_timeout = 2s;
  swr::Redis admin(adminOptions);
  assert(producer.addSingleDouble("read", 7.).ok());
  auto operations = std::make_shared<OperationQueue>();
  ServerConfig serverConfig;
  serverConfig.instance = "registry-runtime";
  serverConfig.nameSpace = "TEST";
  PVConfig config;
  config.name = "blocked";
  config.read = {"default", "read"};
  config.write = RouteConfig{"writer", "command"};
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1, operations);
  runtime->activate();
  auto unrelated = std::make_shared<pvxs::server::SharedPV>(pvxs::server::SharedPV::buildReadonly());
  auto value = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  value["value"] = 99;
  unrelated->open(value);
  const PVBinding binding{runtime->sharedPV(), runtime, {}}, other{*unrelated, unrelated, {}};
  auto registry = std::make_shared<PVRegistry>();
  const auto initial = registry->prepare({{"TEST:blocked", binding}, {"TEST:old", binding},
                                         {"TEST:unrelated", other}});
  std::string error;
  assert(registry->publish(initial, {}, error));
  registry->finish(initial);
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  server.addSource("registry", registry).start();
  auto client = server.clientConfig().build();
  assert(client.get("TEST:old").exec()->wait(2.)["value"].as<double>() == 7.);

  auto changed = config;
  changed.transform = LinearTransformConfig{2., 0.};
  auto update = runtime->prepareReconfigure(changed, 2);
  const auto next = registry->prepare({{"TEST:blocked", binding}, {"TEST:new", binding},
                                      {"TEST:unrelated", other}});
  const auto earlierXadds = clientsExecuting(admin, "xadd");
  admin.command<void>("CLIENT", "PAUSE", 5000, "WRITE");
  auto write = client.put("TEST:old").set("value", 6.).exec();
  eventually([&] {
    for (const auto& id : clientsExecuting(admin, "xadd"))
      if (!earlierXadds.count(id)) return true;
    return false;
  });
  assert(operations->stats().running == 1);

  std::promise<void> entered;
  auto started = entered.get_future();
  auto publication = std::async(std::launch::async, [&] {
    return registry->publish(next, [&](std::string&) {
      entered.set_value();
      update->commit(); // waits behind the mutex held across blocked writeToRedis
      return true;
    }, error);
  });
  assert(started.wait_for(3s) == std::future_status::ready);
  auto listing = std::async(std::launch::async, [&] { return registry->onList(); });
  auto fresh = server.clientConfig().build();
  bool available = false;
  try {
    available = fresh.get("TEST:unrelated").exec()->wait(1.)["value"].as<int>() == 99 &&
        fresh.get("TEST:blocked").exec()->wait(1.)["value"].as<double>() == 7.;
  } catch (const std::exception&) {}
  const bool listed = listing.wait_for(1s) == std::future_status::ready;
  const bool waitingOnRuntime = publication.wait_for(0s) == std::future_status::timeout;
  admin.command<void>("CLIENT", "UNPAUSE");
  assert(publication.get());
  const auto names = listing.get().names;
  assert(available && listed && waitingOnRuntime);
  assert(names->count("TEST:old") && !names->count("TEST:new"));
  write->wait(2.);
  registry->finish(next);
  update->refresh();
  assert(fresh.get("TEST:new").exec()->wait(2.)["value"].as<double>() == 14.);
  const auto history = producer.getValues<double>("command");
  assert(history.size() == 1 && history.front().second == 6.);
  fresh.close(); client.close();
  registry->clear();
  runtime->deactivate("done");
  operations->shutdown();
  server.stop();
}
