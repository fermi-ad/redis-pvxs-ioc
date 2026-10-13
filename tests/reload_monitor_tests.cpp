#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/monitor_sample.h"
#include "RedisAdapter.hpp"
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <sys/socket.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

pvxs::Value next(const std::shared_ptr<pvxs::client::Subscription>& monitor) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  for (;;) {
    if (auto value = monitor->pop()) return value;
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(2ms);
  }
}

template<bool Array> void exercise(RedisAdapter& producer, const RedisBackendRegistry& backends,
                                   const ServerConfig& serverConfig, pvxs::server::Server& server,
                                   pvxs::client::Context& client) {
  PVConfig config;
  config.name = Array ? "array" : "scalar";
  config.shape = Array ? Shape::Array : Shape::Scalar;
  config.read = {"default", config.name};
  const auto send = [&](double id) {
    const auto result = Array ? producer.addSingleList(config.name, std::vector<double>{id, id + 1})
                              : producer.addSingleDouble(config.name, id);
    assert(result.ok());
  };
  const auto id = [](const pvxs::Value& value) {
    if constexpr (Array) return value["value"].as<pvxs::shared_array<const double>>()[0];
    else return value["value"].as<double>();
  };
  send(1.);
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1);
  runtime->activate();
  server.addPV(runtime->fullName(), runtime->sharedPV());
  auto monitor = client.monitor(runtime->fullName()).record("queueSize", uint32_t(16))
      .record("pipeline", true).maskConnected(true).maskDisconnected(false)
      .event([](pvxs::client::Subscription&) {}).exec();
  const pvxs::Value initial = next(monitor);
  assert(id(initial) == 1. && monitorPublishesSample(initial));
  const pvxs::Value stamp = initial["timeStamp"].clone();

  config.metadata.description = "changed while acquisition is paused";
  runtime->reconfigure(config, 2);
  const pvxs::Value metadata = next(monitor);
  assert(id(metadata) == 1.);
  assert(metadata["display.description"].as<std::string>() == config.metadata.description);
  assert(metadata["display.description"].isMarked());
  assert(metadata["timeStamp.secondsPastEpoch"].as<int64_t>() == stamp["secondsPastEpoch"].as<int64_t>());
  assert(metadata["timeStamp.nanoseconds"].as<int32_t>() == stamp["nanoseconds"].as<int32_t>());
  assert(!monitorPublishesSample(metadata));

  // Post the exact same acquired sample deliberately. Both field-mark variants
  // must remain visible to duplicate accounting, despite identical contents.
  auto duplicate = runtime->sharedPV().fetch();
  duplicate.unmark(); duplicate["value"].mark(); runtime->sharedPV().post(duplicate);
  pvxs::Value repeated = next(monitor);
  assert(id(repeated) == 1. && monitorPublishesSample(repeated));
  duplicate.unmark(); duplicate["timeStamp.nanoseconds"].mark(); runtime->sharedPV().post(duplicate);
  repeated = next(monitor);
  assert(id(repeated) == 1. && monitorPublishesSample(repeated));

  // A new Redis entry can have equal contents and is still a new acquisition.
  send(1.);
  const pvxs::Value acquired = next(monitor);
  assert(id(acquired) == 1. && monitorPublishesSample(acquired));

  // Transform changes publish the new rendered payload while retaining the
  // acquisition timestamp. Small changes must not use confirmation tolerance.
  config.transform = LinearTransformConfig{1., 1e-15};
  runtime->reconfigure(config, 3);
  const pvxs::Value transformed = next(monitor);
  assert(id(transformed) != 1. && monitorPublishesSample(transformed));
  assert(transformed["value"].isMarked());
  assert(!transformed["timeStamp"].isMarked(true, true));

  send(2.);
  const pvxs::Value updated = next(monitor);
  assert(id(updated) > 2. && monitorPublishesSample(updated));
  // Preparing metadata before a new sample must never post the staged sample.
  config.metadata.description = "prepared before newest acquisition";
  auto staged = runtime->prepareReconfigure(config, 4);
  send(3.);
  const pvxs::Value newest = next(monitor);
  staged->commit(); staged->refresh();
  const pvxs::Value refreshed = next(monitor);
  assert(id(refreshed) == id(newest) && !monitorPublishesSample(refreshed));
  assert(refreshed["display.description"].as<std::string>() == config.metadata.description);

  send(0.); next(monitor);
  config.transform.reset(); runtime->reconfigure(config, 5);
  const pvxs::Value zero = next(monitor);
  assert(id(zero) == 0. && !std::signbit(id(zero)));
  config.transform = LinearTransformConfig{-1., -0.}; runtime->reconfigure(config, 6);
  const pvxs::Value negativeZero = next(monitor);
  assert(id(negativeZero) == 0. && std::signbit(id(negativeZero)));
  assert(monitorPublishesSample(negativeZero));

  monitor->cancel(); server.removePV(runtime->fullName()); runtime->deactivate("test finished");
  std::cout << config.name << ": metadata excluded; marked duplicates and new acquisitions retained\n";
}

int main() {
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  auto adapter = std::make_shared<RedisAdapter>("reload-monitor-tests", options);
  RedisAdapter producer("reload-monitor-tests", options);
  RedisBackendRegistry backends{{"default", adapter}};
  ServerConfig config; config.instance = "reload-monitor-tests"; config.nameSpace = "TEST";
  auto server = pvxs::server::Config::isolated(AF_INET).build(); server.start();
  auto client = server.clientConfig().build();
  exercise<false>(producer, backends, config, server, client);
  exercise<true>(producer, backends, config, server, client);
  client.close(); server.stop();
}
