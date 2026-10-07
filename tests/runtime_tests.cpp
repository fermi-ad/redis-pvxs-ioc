#include "redis_pvxs_ioc/runtime.h"
#include "RedisAdapter.hpp"
#include <alarm.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/socket.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class Predicate> void eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  assert(predicate());
}

struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::string error;
  void complete(pvxs::client::Result&& result) {
    std::string failure;
    try { result(); } catch (const std::exception& ex) { failure = ex.what(); }
    std::lock_guard<std::mutex> guard(mutex);
    error = failure; done = true; changed.notify_all();
  }
  bool waitFor(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, duration, [&] { return done; });
  }
};

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  RA_Options options;
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  options.cxn.port = std::stoi(port);
  auto adapter = std::make_shared<RedisAdapter>("runtime-tests", options);
  RedisAdapter producer("runtime-tests", options);
  assert(adapter->connected());
  RedisBackendRegistry backends{{"default", adapter}};
  ServerConfig serverConfig;
  serverConfig.instance = "runtime-tests";
  serverConfig.nameSpace = "TEST";
  PVConfig config;
  config.name = "value";
  config.read = {"default", "readback"};
  config.write = RouteConfig{"default", "command"};
  config.confirm = ConfirmConfig{"default", "ack", 1500};
  assert(producer.addSingleDouble("readback", 0.).ok());
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1);
  runtime->activate();
  server.addPV(runtime->fullName(), runtime->sharedPV()).start();
  auto client = server.clientConfig().build();
  assert(client.get("TEST:value").exec()->wait(3.)["value"].as<double>() == 0.);

  Completion completion;
  auto operation = client.put("TEST:value").set("value", 123.)
      .result([&](pvxs::client::Result&& result) { completion.complete(std::move(result)); }).exec();
  eventually([&] {
    double value = 0.;
    return producer.getSingleValue<double>("command", value).ok() && value == 123.;
  });
  assert(producer.addSingleDouble("readback", 123.).ok());
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 123.; });
  assert(!completion.waitFor(100ms));
  assert(producer.addSingleDouble("ack", 123.).ok());
  assert(completion.waitFor(3s) && completion.error.empty());

  // The second subscriber runs after the runtime's confirmation subscriber.
  std::mutex ackMutex;
  std::condition_variable ackChanged;
  std::string ackId;
  auto observer = adapter->subscribeStream("ack", [&](const auto&, const auto&, const auto& batch) {
    std::lock_guard<std::mutex> guard(ackMutex);
    ackId = batch.back().first; ackChanged.notify_all();
  });
  auto otherAck = producer.addSingleDouble("ack", 456.);
  {
    std::unique_lock<std::mutex> lock(ackMutex);
    assert(ackChanged.wait_for(lock, 3s, [&] { return ackId == otherAck.id(); }));
  }
  auto metadata = config;
  metadata.metadata.description = "new description";
  runtime->reconfigure(metadata, 2);
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 123.);
  assert(runtime->sharedPV().fetch()["display.description"].as<std::string>() == "new description");

  // A matching observation that predates the command, and metadata replay of
  // cached readback, must not satisfy a new confirmed write.
  auto staleAck = producer.addSingleDouble("ack", 123.);
  {
    std::unique_lock<std::mutex> lock(ackMutex);
    assert(ackChanged.wait_for(lock, 3s, [&] { return ackId == staleAck.id(); }));
  }
  auto previousCommand = producer.getStreamSnapshot("command").id;
  Completion repeated;
  auto repeatedOperation = client.put("TEST:value").set("value", 123.)
      .result([&](pvxs::client::Result&& result) { repeated.complete(std::move(result)); }).exec();
  eventually([&] { return producer.getStreamSnapshot("command").id != previousCommand; });
  metadata.metadata.description = "metadata during pending write";
  runtime->reconfigure(metadata, 3);
  assert(!repeated.waitFor(100ms));
  assert(producer.addSingleDouble("ack", 123.).ok());
  assert(repeated.waitFor(3s) && repeated.error.empty());

  // Malformed source input keeps the last value and marks it invalid, then recovers.
  assert(producer.addSingleValue<RedisAdapter::Attrs>("readback", {{"_", "x"}}).ok());
  eventually([&] { return runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid; });
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 123.);
  assert(producer.addSingleDouble("readback", 7.).ok());
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 7.; });
  assert(runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);

  observer.reset();
  auto replacementConfig = config;
  replacementConfig.confirm.reset();
  replacementConfig.write = RouteConfig{"default", "new-command"};
  adapter->setDeferReaders(true);
  auto replacement = makeRuntime(serverConfig, replacementConfig, backends, {}, 3);
  adapter->setDeferReaders(false);
  server.removePV(runtime->fullName());
  runtime->deactivate("replaced");
  replacement->activate();
  server.addPV(replacement->fullName(), replacement->sharedPV());
  assert(producer.addSingleDouble("readback", 8.).ok());
  eventually([&] { return replacement->sharedPV().fetch()["value"].as<double>() == 8.; });
  {
    auto rejected = makeRuntime(serverConfig, replacementConfig, backends, {}, 4);
    rejected->deactivate("staging rejected");
  }
  assert(producer.addSingleDouble("readback", 9.).ok());
  eventually([&] { return replacement->sharedPV().fetch()["value"].as<double>() == 9.; });

  PVConfig gapConfig;
  gapConfig.name = "gap";
  gapConfig.read = {"default", "gap"};
  assert(producer.addSingleDouble("gap", 1.).ok());
  adapter->setDeferReaders(true);
  auto gap = makeRuntime(serverConfig, gapConfig, backends, {}, 1);
  assert(producer.addSingleDouble("gap", 2.).ok());
  adapter->setDeferReaders(false);
  gap->activate();
  eventually([&] { return gap->sharedPV().fetch()["value"].as<double>() == 2.; });

  PVConfig emptyConfig;
  emptyConfig.name = "empty";
  emptyConfig.type = PrimitiveType::Float64;
  emptyConfig.shape = Shape::Array;
  emptyConfig.read = {"default", "empty"};
  assert(producer.addSingleList<double>("empty", std::vector<double>{}).ok());
  auto empty = makeRuntime(serverConfig, emptyConfig, backends, {}, 1);
  assert(empty->sharedPV().fetch()["value"].as<pvxs::shared_array<const double>>().empty());
  assert(empty->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);

  PVConfig missingConfig;
  missingConfig.name = "missing";
  missingConfig.read = {"default", "missing"};
  missingConfig.initialValue = 10.;
  auto missing = makeRuntime(serverConfig, missingConfig, backends, {}, 1);
  assert(missing->sharedPV().fetch()["value"].as<double>() == 10.);
  assert(missing->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(missing->sharedPV().fetch()["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);

  missing->deactivate("done"); empty->deactivate("done"); gap->deactivate("done");
  client.close(); server.stop(); replacement->deactivate("done");
  std::cout << "runtime read/confirm isolation, replacement, invalid input and snapshot tests passed\n";
}
