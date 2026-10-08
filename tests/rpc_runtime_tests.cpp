#include "redis_pvxs_ioc/rpc_pv.h"
#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/pv_registry.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
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
  const auto end = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < end);
    std::this_thread::sleep_for(2ms);
  }
}
struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::string error, token;
  void result(pvxs::client::Result&& result) {
    std::lock_guard<std::mutex> guard(mutex);
    try { token = result()["token"].as<std::string>(); }
    catch (const std::exception& ex) { error = ex.what(); }
    done = true; changed.notify_all();
  }
  void wait() {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 3s, [&] { return done; }));
  }
};

int main(int argc, char** argv) {
  assert(argc == 2);
  using pvxs::TypeCode; using pvxs::Member;
  const auto requestType = pvxs::TypeDef(TypeCode::Struct, {
    Member(TypeCode::Int32, "delay_ms"), Member(TypeCode::String, "token"), Member(TypeCode::Bool, "fail")});
  auto bridge = std::make_shared<GrpcBridge>("127.0.0.1:" + std::string(argv[1]));
  bridge->discover("redis_pvxs_test.Control");
  auto queue = std::make_shared<OperationQueue>(OperationQueueLimits{2, 2, 8192});
  auto stats = std::make_shared<RpcCallStats>();
  const auto runtime = [&](const char* name, const char* method, uint32_t timeout = 5000) {
    return std::make_shared<RpcPV>(bridge, BridgeMethod{"redis_pvxs_test.Control", method},
                                  GrpcBridge::Fields{}, name, queue, timeout, stats);
  };
  auto wait = runtime("RPC:wait", "Wait"), inspect = runtime("RPC:inspect", "Inspect");
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  auto registry = std::make_shared<PVRegistry>();
  PVBindings bindings{{"RPC:wait", {wait->sharedPV(), wait, {}}}, {"RPC:inspect", {inspect->sharedPV(), inspect, {}}}};
  std::string error;
  const auto publish = [&] {
    auto prepared = registry->prepare(bindings);
    assert(registry->publish(prepared, [](std::string&) { return true; }, error));
    registry->finish(prepared);
  };
  publish(); server.addSource("registry", registry); server.start();
  auto client = server.clientConfig().build();
  const auto counts = [&] { return client.rpc("RPC:inspect", pvxs::TypeDef(TypeCode::Struct, {}).create()).exec()->wait(.5); };
  const auto count = [&](const char* name) { return counts()[name].as<uint64_t>(); };
  const auto call = [&](const char* name, int delay, std::string token, Completion& completion, bool fail = false) {
    auto value = requestType.create(); value["delay_ms"] = int32_t(delay);
    value["token"] = token; value["fail"] = fail;
    return client.rpc(name, value).result([&completion](pvxs::client::Result&& result) {
      completion.result(std::move(result));
    }).exec();
  };
  Completion first, second, third, overloaded;
  auto firstOp = call("RPC:wait", 5000, "first", first);
  eventually([&] { return count("started") == 1; }); // unrelated RPC remains responsive
  auto secondOp = call("RPC:wait", 0, "second", second);
  auto thirdOp = call("RPC:wait", 0, "third", third);
  eventually([&] { return queue->stats().queued == 2; });
  auto overloadedOp = call("RPC:wait", 0, "overloaded", overloaded);
  overloaded.wait(); assert(overloaded.error.find("overloaded") != std::string::npos);
  secondOp->cancel(); eventually([&] { return queue->stats().queued == 1; });
  firstOp->cancel(); third.wait(); assert(third.error.empty() && third.token == "third");
  eventually([&] { return count("cancelled") == 1; });
  assert(count("started") == 2); // cancelled queued command never reached gRPC

  Completion bytes;
  auto bytesOp = call("RPC:wait", 0, std::string(9000, 'x'), bytes);
  bytes.wait(); assert(bytes.error.find("overloaded") != std::string::npos);
  assert(count("started") == 2);

  // The second request spends most of its total deadline in the queue.
  auto timed = runtime("RPC:timed", "Wait", 400);
  bindings.emplace("RPC:timed", PVBinding{timed->sharedPV(), timed, {}}); publish();
  Completion ahead, expired;
  const auto begin = std::chrono::steady_clock::now();
  auto aheadOp = call("RPC:timed", 250, "ahead", ahead);
  eventually([&] { return count("started") == 3; });
  auto expiredOp = call("RPC:timed", 2000, "expired", expired);
  ahead.wait(); assert(ahead.error.empty()); expired.wait();
  assert(expired.error.find("deadline") != std::string::npos || expired.error.find("Deadline") != std::string::npos);
  assert(std::chrono::steady_clock::now() - begin < 900ms);
  eventually([&] { return count("cancelled") == 2; });
  assert(count("started") == 4);

  // An UNAVAILABLE status after backend acceptance must not replay a command.
  Completion ambiguous;
  auto ambiguousOp = call("RPC:wait", 0, "accepted", ambiguous, true);
  ambiguous.wait(); assert(ambiguous.error.find("do not replay") != std::string::npos);
  assert(count("failed") == 1 && count("started") == 5);

  // Endpoint retirement closes channels and cancels both active and queued work.
  auto retired = runtime("RPC:retired", "Wait");
  bindings.emplace("RPC:retired", PVBinding{retired->sharedPV(), retired, {}}); publish();
  Completion retiring, neverSent;
  auto retiringOp = call("RPC:retired", 5000, "retiring", retiring);
  eventually([&] { return count("started") == 6; });
  auto neverOp = call("RPC:retired", 0, "never", neverSent);
  eventually([&] { return queue->stats().queued == 1; });
  bindings.erase("RPC:retired"); publish();
  eventually([&] { return count("cancelled") == 3; });
  assert(count("started") == 6);

  const auto policy = std::filesystem::temp_directory_path() / ("rpc-access-" + std::to_string(getpid()) + ".acf");
  const auto writePolicy = [&](const char* rights) { std::ofstream(policy) << "ASG(CONTROL) { RULE(0, " << rights << ") }\n"; };
  writePolicy("WRITE");
  AccessConfig config; config.enabled = true; config.file = policy.string();
  auto access = std::make_shared<AccessController>(config);
  assert(access->start({"CONTROL"}, error));
  auto secured = runtime("RPC:secured", "Wait");
  access->addPV("RPC:secured", secured->sharedPV(), {"CONTROL", 0});
  server.addSource("secured", access->source());
  Completion allowed, revoked;
  auto allowedOp = call("RPC:secured", 5000, "allowed", allowed);
  eventually([&] { return count("started") == 7; });
  auto revokedOp = call("RPC:secured", 0, "revoked", revoked);
  eventually([&] { return queue->stats().queued == 1; });
  writePolicy("NONE"); assert(access->reload("revoke", error));
  eventually([&] { return count("cancelled") == 4; });
  assert(count("started") == 7);
  allowedOp->cancel(); revokedOp->cancel(); retiringOp->cancel(); neverOp->cancel();

  Completion shuttingDown;
  auto shutdownOp = call("RPC:wait", 5000, "shutdown", shuttingDown);
  eventually([&] { return count("started") == 8; });
  const auto shutdownStart = std::chrono::steady_clock::now();
  queue->shutdown(); shuttingDown.wait();
  assert(std::chrono::steady_clock::now() - shutdownStart < 1s);
  assert(queue->stats().residentBytes == 0);
  client.close(); access->clearBindings(); server.removeSource("secured"); registry->clear(); server.stop();
  std::filesystem::remove(policy);
  assert(stats->succeeded > 0 && stats->failed >= 1);
  std::cout << "bounded asynchronous RPC, cancellation, retirement, ACF revocation, total deadlines and no replay passed\n";
}
