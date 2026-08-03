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
#include <sys/socket.h>

#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;

int main() {
  // Match Application startup ordering: initialize PVXS before asLib.
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  const auto policyPath = std::filesystem::temp_directory_path() / "redis-pvxs-ioc-runtime.acf";
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
  mailbox.onPut([](pvxs::server::SharedPV& pv,
                   std::unique_ptr<pvxs::server::ExecOp>&& op,
                   pvxs::Value&& value) {
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

  writePolicy(
      "ASG(TEST) { RULE(0, WRITE, TRAPWRITE) }\n"
      "ASG(RPC) { RULE(0, WRITE) }\n"
      "ASG(DENIED_RPC) { RULE(0, NONE) }\n");
  assert(controller->reload("test-restore", error));
  client.put("secured").set("value", 9).exec()->wait(5.0);
  assert(client.get("secured").exec()->wait(5.0)["value"].as<int32_t>() == 9);
  assert(controller->status().generation == 3u);

  server.stop();
  std::error_code ignored;
  std::filesystem::remove(policyPath, ignored);
  return 0;
}
