// A TRAPWRITE write admitted under audit id N and then refused by the dispatch
// re-check must keep id N on its denial record, so the audit trail does not
// read as an ordinary backend error.
#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/operation_queue.h"
#include "redis_pvxs_ioc/access_control.h"
#include "RedisAdapter.hpp"
#include "audit_trail.h"
#include <asLib.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    assert(changed.wait_for(lock, 5s, [&] { return done; }));
  }
};

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  auto adapter = std::make_shared<RedisAdapter>("dispatchdenial", options);
  RedisAdapter producer("dispatchdenial", options);
  RedisBackendRegistry backends{{"default", adapter}};
  auto queue = std::make_shared<OperationQueue>(OperationQueueLimits{2, 2, 4096});

  ServerConfig serverConfig;
  serverConfig.instance = "dispatchdenial";
  serverConfig.nameSpace = "TEST";
  PVConfig config;
  config.name = "secured";
  config.read = {"default", "read"};
  config.write = RouteConfig{"default", "command"};
  config.confirm = ConfirmConfig{"default", "ack", 5000};
  assert(producer.addSingleDouble("read", 1.).ok());
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1, queue);
  runtime->activate();

  const auto policy = std::filesystem::temp_directory_path() /
      ("redis-pvxs-dispatch-denial-" + std::to_string(getpid()) + ".acf");
  std::ofstream(policy) << "ASG(WRITE) { RULE(1, WRITE, TRAPWRITE) }\n";
  AccessConfig accessConfig;
  accessConfig.enabled = true;
  accessConfig.file = policy.string();
  auto access = std::make_shared<AccessController>(accessConfig);
  std::string error;
  assert(access->start({"WRITE"}, error));
  access->addPV("TEST:secured", runtime->sharedPV(), {"WRITE", 0});
  server.addSource("secured", access->source());
  server.start();
  auto client = server.clientConfig().build();
  const auto put = [&](double value, Completion& result) {
    return client.put("TEST:secured").set("value", value).result([&result](pvxs::client::Result&& done) {
      result.result(std::move(done));
    }).exec();
  };

  CapturedStderr captured;
  // The first write reaches Redis and waits for confirmation, holding the
  // PV's lane; the second is admitted under its own audit id and queues.
  Completion first, second;
  auto firstOp = put(10., first);
  eventually([&] { return producer.getValues<double>("command").size() == 1; });
  auto secondOp = put(20., second);
  eventually([&] { return queue->stats().queued == 1; });

  // Revoke WRITE in Base. This fires the rights-change callback the controller
  // sees during policy activation, which clears cached rights at once. The
  // channel recompute/close that would cancel the queued write is deliberately
  // not run yet, so the second write dispatches inside that window.
  assert(asInitMem("ASG(WRITE) { RULE(1, READ) }\n", nullptr) == 0);
  assert(producer.addSingleDouble("ack", 10.).ok());
  first.wait();
  second.wait();
  const auto records = auditRecords(captured.finish());
  for (const auto& record : records)
    std::cout << "id=" << record.id << " phase=" << record.phase << " result=" << record.result << "\n";
  std::cout << "second put: " << (second.error.empty() ? "success" : second.error) << std::endl;

  // The refused write never reached Redis, and its client saw the dispatch denial.
  assert(first.error.empty());
  assert(producer.getValues<double>("command").size() == 1);
  assert(second.error.find("no longer authorized at dispatch") != std::string::npos);

  std::vector<std::string> admitted;
  for (const auto& record : records)
    if (record.phase == "authorization" && record.result == "allowed") admitted.push_back(record.id);
  assert(admitted.size() == 2);
  if (!deniedAtDispatch(records, admitted[1], "put")) {
    std::cout << "FAIL: dispatch denial is not recorded under admitted id " << admitted[1] << std::endl;
    return 1;
  }
  const auto status = access->status();
  assert(status.operationsDenied == 1 && status.operationsFailed == 0 && status.operationsInFlight == 0);

  firstOp->cancel(); secondOp->cancel();
  runtime->deactivate("done");
  queue->shutdown();
  client.close();
  server.removeSource("secured");
  server.stop();
  std::filesystem::remove(policy);
  std::cout << "dispatch denial keeps the admitted operation id\n";
}
