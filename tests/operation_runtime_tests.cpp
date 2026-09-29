#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/operation_queue.h"
#include "redis_pvxs_ioc/access_control.h"
#include "RedisAdapter.hpp"
#include <alarm.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <pvxs/client.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(2ms);
  }
}
struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::string error;
  void result(pvxs::client::Result&& result) {
    std::string message;
    try { result(); } catch (const std::exception& ex) { message = ex.what(); }
    std::lock_guard<std::mutex> guard(mutex);
    error = message; done = true; changed.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 3s, [&] { return done; }));
  }
};

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  auto adapter = std::make_shared<RedisAdapter>("operations", options);
  RedisAdapter producer("operations", options);
  RedisBackendRegistry backends{{"default", adapter}};
  auto queue = std::make_shared<OperationQueue>(OperationQueueLimits{2, 2, 4096});
  ServerConfig serverConfig;
  serverConfig.instance = "operations";
  serverConfig.nameSpace = "TEST";
  PVConfig config;
  config.name = "value";
  config.read = {"default", "read"};
  config.write = RouteConfig{"default", "command"};
  config.confirm = ConfirmConfig{"default", "ack", 5000};
  assert(producer.addSingleDouble("read", 7.).ok());
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1, queue);
  runtime->activate();
  server.addPV("TEST:value", runtime->sharedPV());
  server.addPV("TEST:alias", runtime->sharedPV());
  server.start();
  auto client = server.clientConfig().build();
  const auto put = [&](const std::string& name, double value, Completion& result) {
    return client.put(name).set("value", value).result([&result](pvxs::client::Result&& value) {
      result.result(std::move(value));
    }).exec();
  };
  const auto written = [&](const std::string& key, double expected) {
    double value = 0.;
    return producer.getSingleValue<double>(key, value).ok() && value == expected;
  };

  Completion first, second, third, overload;
  auto firstOp = put("TEST:value", 1., first);
  eventually([&] { return written("command", 1.); });
  assert(client.get("TEST:alias").exec()->wait(.5)["value"].as<double>() == 7.);
  auto secondOp = put("TEST:alias", 2., second);
  auto thirdOp = put("TEST:value", 3., third);
  eventually([&] { return queue->stats().queued == 2; });
  auto overloadedOp = put("TEST:alias", 4., overload);
  overload.wait();
  assert(overload.error.find("overloaded") != std::string::npos);
  secondOp->cancel();
  eventually([&] { return queue->stats().queued == 1; });
  assert(written("command", 1.));
  assert(producer.addSingleDouble("ack", 1.).ok());
  first.wait();
  assert(first.error.empty());
  eventually([&] { return written("command", 3.); });
  thirdOp->cancel();
  eventually([&] { return queue->stats().running == 0 && queue->stats().queued == 0; });
  const auto history = producer.getValues<double>("command");
  assert(history.size() == 2 && history.front().second == 1. && history.back().second == 3.);

  // A finite total deadline ends confirmation waiting without another write.
  auto timeoutConfig = config;
  timeoutConfig.name = "timeout";
  timeoutConfig.write->key = "timeout-command";
  OperationLimitsConfig timeoutLimits;
  timeoutLimits.operationTimeoutMs = 100;
  auto timeout = makeRuntime(serverConfig, timeoutConfig, backends, {}, 1, queue, timeoutLimits);
  timeout->activate();
  server.addPV("TEST:timeout", timeout->sharedPV());
  Completion timed;
  auto timedOp = put("TEST:timeout", 8., timed);
  timed.wait();
  assert(timed.error.find("deadline") != std::string::npos);
  eventually([&] { return queue->stats().running == 0; });
  assert(producer.getValues<double>("timeout-command").size() == 1);

  // Reject oversized arrays before queueing or touching Redis, and retain the
  // last good readback when oversized source data arrives.
  auto arrayConfig = config;
  arrayConfig.name = "array";
  arrayConfig.type = PrimitiveType::UInt8;
  arrayConfig.shape = Shape::Array;
  arrayConfig.read.key = "array-read";
  arrayConfig.write->key = "array-command";
  arrayConfig.confirm.reset();
  OperationLimitsConfig payloadLimits;
  payloadLimits.maxPayloadBytes = 16;
  assert(producer.addSingleList<uint8_t>("array-read", std::vector<uint8_t>{1, 2}).ok());
  auto array = makeRuntime(serverConfig, arrayConfig, backends, {}, 1, queue, payloadLimits);
  array->activate();
  server.addPV("TEST:array", array->sharedPV());
  pvxs::shared_array<uint8_t> large(32);
  bool rejected = false;
  try { client.put("TEST:array").set("value", large.freeze()).exec()->wait(1.); }
  catch (const pvxs::client::RemoteError& error) { rejected = std::string(error.what()).find("max_payload_bytes") != std::string::npos; }
  assert(rejected && !producer.getStreamSnapshot("array-command").present());
  assert(producer.addSingleList<uint8_t>("array-read", std::vector<uint8_t>(32)).ok());
  eventually([&] { return array->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid; });
  assert(array->sharedPV().fetch()["value"].as<pvxs::shared_array<const uint8_t>>().size() == 2);

  // Revocation cancels queued work and removes dispatch authority.
  const auto policy = std::filesystem::temp_directory_path() / ("redis-pvxs-operations-" + std::to_string(getpid()) + ".acf");
  const auto writePolicy = [&](const char* rights) {
    std::ofstream file(policy);
    file << "ASG(WRITE) { RULE(0, " << rights << ") }\n";
  };
  writePolicy("WRITE");
  AccessConfig accessConfig;
  accessConfig.enabled = true;
  accessConfig.file = policy.string();
  auto access = std::make_shared<AccessController>(accessConfig);
  std::string error;
  assert(access->start({"WRITE"}, error));
  auto securedConfig = config;
  securedConfig.name = "secured";
  securedConfig.write->key = "secured-command";
  auto secured = makeRuntime(serverConfig, securedConfig, backends, {}, 1, queue);
  secured->activate();
  access->addPV("TEST:secured", secured->sharedPV(), {"WRITE", 0});
  server.addSource("secured", access->source());
  Completion permitted, revoked;
  auto permittedOp = put("TEST:secured", 10., permitted);
  eventually([&] { return written("secured-command", 10.); });
  auto revokedOp = put("TEST:secured", 20., revoked);
  eventually([&] { return queue->stats().queued == 1; });
  writePolicy("NONE");
  assert(access->reload("revoke", error));
  eventually([&] { return queue->stats().queued == 0 && queue->stats().running == 0; });
  assert(producer.getValues<double>("secured-command").size() == 1);
  permittedOp->cancel(); revokedOp->cancel();

  secured->deactivate("done"); array->deactivate("done"); timeout->deactivate("done"); runtime->deactivate("done");
  queue->shutdown();
  client.close();
  server.removeSource("secured");
  server.stop();
  std::filesystem::remove(policy);
}
