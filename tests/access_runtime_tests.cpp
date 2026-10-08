#include "redis_pvxs_ioc/access_control.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

int main() {
  // Match Application startup ordering: initialize PVXS before asLib.
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  const auto policyPath = std::filesystem::temp_directory_path() / ("redis-pvxs-ioc-runtime-" + std::to_string(getpid()) + ".acf");
  const auto writePolicy = [&](const std::string& text) {
    std::ofstream output(policyPath, std::ios::binary | std::ios::trunc);
    output << text;
    assert(output.good());
  };
  writePolicy(
      "ASG(TEST) { RULE(0, READ) }\n"
      "ASG(RPC) { RULE(0, WRITE) }\n"
      "ASG(DENIED_RPC) { RULE(0, NONE) }\n");

  AccessConfig config;
  config.enabled = true;
  config.file = policyPath.string();
  std::string error;
  auto controller = std::make_shared<AccessController>(config);
  assert(controller->start({"TEST", "RPC", "DENIED_RPC"}, error));

  auto mailbox = pvxs::server::SharedPV::buildMailbox();
  auto initial = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  initial["value"] = static_cast<int32_t>(7);
  std::mutex pendingMutex;
  std::unique_ptr<pvxs::server::ExecOp> pending;
  mailbox.onPut([&](pvxs::server::SharedPV& pv,
                   std::unique_ptr<pvxs::server::ExecOp>&& op,
                   pvxs::Value&& value) {
    const auto request = value["value"].as<int32_t>();
    if (request == 99) { op->error("backend fixture rejected write"); return; }
    if (request == 100) { std::lock_guard<std::mutex> lock(pendingMutex); pending = std::move(op); return; }
    if (request == 101) return; // Dropped by the handler without completion.
    pv.post(value);
    op->reply();
  });
  mailbox.open(initial);
  controller->addPV("secured", mailbox, AccessAssignment{"TEST", 0});

  std::atomic<uint64_t> rpcCalls{0u};
  auto rpc = pvxs::server::SharedPV::buildReadonly();
  rpc.onRPC([&](pvxs::server::SharedPV&,
                std::unique_ptr<pvxs::server::ExecOp>&& op,
                pvxs::Value&& request) {
    rpcCalls.fetch_add(1u, std::memory_order_relaxed);
    op->reply(request);
  });
  controller->addPV("secured-rpc", rpc, AccessAssignment{"RPC", 0});
  controller->addPV("denied-rpc", rpc, AccessAssignment{"DENIED_RPC", 0});
  server.addSource("access", controller->source()).start();
  auto client = server.clientConfig().build();

  assert(client.get("secured").exec()->wait(5.0)["value"].as<int32_t>() == 7);
  auto rpcRequest = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  rpcRequest["value"] = static_cast<int32_t>(12);
  assert(client.rpc("secured-rpc", rpcRequest.clone()).exec()->wait(5.0)["value"].as<int32_t>() == 12);
  bool rpcDenied = false;
  try {
    client.rpc("denied-rpc", rpcRequest.clone()).exec()->wait(5.0);
  } catch (const pvxs::client::RemoteError& ex) {
    rpcDenied = std::string(ex.what()).find("access denied") != std::string::npos;
  }
  assert(rpcDenied);
  assert(rpcCalls.load(std::memory_order_relaxed) == 1u);
  assert(controller->status().deniedWrites == 1u);
  bool denied = false;
  try {
    client.put("secured").set("value", 8).exec()->wait(5.0);
  } catch (const pvxs::client::RemoteError& ex) {
    denied = std::string(ex.what()).find("access denied") != std::string::npos;
  }
  assert(denied);
  assert(controller->status().deniedWrites == 2u);

  std::mutex monitorMutex;
  std::condition_variable monitorEvent;
  auto monitor = client.monitor("secured")
      .maskConnected(true)
      .maskDisconnected(false)
      .event([&](pvxs::client::Subscription&) { monitorEvent.notify_all(); })
      .exec();
  client.hurryUp();
  {
    std::unique_lock<std::mutex> guard(monitorMutex);
    monitorEvent.wait_for(guard, std::chrono::seconds(5), [&]() {
      return static_cast<bool>(monitor->pop());
    });
  }

  writePolicy(
      "ASG(TEST) { RULE(0, NONE) }\n"
      "ASG(RPC) { RULE(0, WRITE) }\n"
      "ASG(DENIED_RPC) { RULE(0, NONE) }\n");
  assert(controller->reload("test-deny", error));
  bool infoDenied = false;
  try {
    client.info("secured").exec()->wait(5.0);
  } catch (const pvxs::client::RemoteError& ex) {
    infoDenied = std::string(ex.what()).find("access denied") != std::string::npos;
  }
  assert(infoDenied);
  bool monitorStopped = false;
  const auto monitorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!monitorStopped && std::chrono::steady_clock::now() < monitorDeadline) {
    try {
      monitor->pop();
    } catch (const pvxs::client::Disconnect&) {
      monitorStopped = true;
    } catch (const pvxs::client::RemoteError&) {
      monitorStopped = true;
    }
    if (!monitorStopped) {
      std::unique_lock<std::mutex> guard(monitorMutex);
      monitorEvent.wait_for(guard, std::chrono::milliseconds(50));
    }
  }
  assert(monitorStopped);
  for (int attempt = 0; attempt < 2; ++attempt) {
    try { client.get("secured").exec()->wait(2.); assert(false); }
    catch (const pvxs::client::RemoteError&) {}
  }
  assert(controller->status().denialLogsSuppressed > 0);

  writePolicy(
      "ASG(TEST) { RULE(0, WRITE, TRAPWRITE) }\n"
      "ASG(RPC) { RULE(0, WRITE, TRAPWRITE) }\n"
      "ASG(DENIED_RPC) { RULE(0, NONE) }\n");
  assert(controller->reload("test-restore", error));
  client.put("secured").set("value", 9).exec()->wait(5.0);
  assert(client.get("secured").exec()->wait(5.0)["value"].as<int32_t>() == 9);
  assert(controller->status().generation == 3u);

  eventually([&] { return controller->status().operationsSucceeded == 2; });
  assert(controller->status().authorizedOperations == 2); // Denials did not execute.
  try { client.put("secured").set("value", 99).exec()->wait(2.); assert(false); }
  catch (const pvxs::client::RemoteError& error) { assert(std::string(error.what()).find("backend fixture") != std::string::npos); }
  eventually([&] { return controller->status().operationsFailed == 1; });
  auto cancelled = client.put("secured").set("value", 100).exec();
  eventually([&] { return controller->status().operationsInFlight == 1; });
  cancelled->cancel();
  eventually([&] { return controller->status().operationsCancelled == 1; });
  { std::lock_guard<std::mutex> lock(pendingMutex); pending.reset(); }
  assert(controller->status().operationsInFlight == 0 && controller->status().operationsAbandoned == 0);
  auto abandoned = client.put("secured").set("value", 101).exec();
  eventually([&] { return controller->status().operationsAbandoned == 1; });
  abandoned->cancel();
  auto privateRequest = pvxs::TypeDef(pvxs::TypeCode::Struct, {
    pvxs::Member(pvxs::TypeCode::String, "token")}).create();
  privateRequest["token"] = "private-rpc-value-must-not-be-logged";
  assert(client.rpc("secured-rpc", privateRequest).exec()->wait(2.)["token"].as<std::string>() ==
         "private-rpc-value-must-not-be-logged");
  eventually([&] { return controller->status().operationsSucceeded == 3; });
  const auto status = controller->status();
  assert(status.authorizedOperations == 6 && status.operationsInFlight == 0);
  assert(status.operationsFailed == 1 && status.operationsCancelled == 1 && status.operationsAbandoned == 1);

  monitor->cancel(); client.close();
  eventually([&] { return controller->status().activeClients == 0; });
  // Short-lived peers release their rate-limit state; admission also prunes
  // expired weak client references without waiting for a policy change.
  for (unsigned peer = 0; peer < 12; ++peer) {
    auto transient = server.clientConfig().build();
    try { transient.rpc("denied-rpc", privateRequest).exec()->wait(2.); assert(false); }
    catch (const pvxs::client::RemoteError&) {}
    transient.close();
    eventually([&] { return controller->status().activeClients == 0; });
  }
  controller->clearBindings();
  server.stop();
  eventually([&] { return controller->status().activeClients == 0; });
  std::error_code ignored;
  std::filesystem::remove(policyPath, ignored);
  return 0;
}
