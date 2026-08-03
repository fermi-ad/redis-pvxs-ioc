#include "redis_pvxs_ioc/access_control.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>

#include <asLib.h>
#include <epicsString.h>

#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;

namespace {

void forceHagAddress(const char* group, const char* address) {
  auto* base = const_cast<ASBASE*>(pasbase);
  assert(base);
  HAG* foundGroup = nullptr;
  for (auto* candidate = reinterpret_cast<HAG*>(ellFirst(&base->hagList)); candidate;
       candidate = reinterpret_cast<HAG*>(ellNext(&candidate->node))) {
    if (std::strcmp(candidate->name, group) == 0) {
      foundGroup = candidate;
      break;
    }
  }
  assert(foundGroup);
  auto* host = reinterpret_cast<HAGNAME*>(ellFirst(&foundGroup->list));
  assert(host);
  assert(host->source);
  std::free(host->source);
  host->source = epicsStrDup(address);
  host->expires = 0;
  base->hagExpires = 1;
}

}  // namespace

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  const auto policyPath = std::filesystem::temp_directory_path() /
                          "redis-pvxs-ioc-hag-runtime.acf";
  {
    std::ofstream output(policyPath, std::ios::binary | std::ios::trunc);
    output << "HAG(SITE) { localhost }\n"
              "ASG(HAGGED) { RULE(0, READ) { HAG(SITE) } }\n";
    assert(output.good());
  }

  AccessConfig config;
  config.enabled = true;
  config.file = policyPath.string();
  std::string error;
  auto controller = std::make_shared<AccessController>(config);
  assert(controller->start({"HAGGED"}, error));

  auto mailbox = pvxs::server::SharedPV::buildReadonly();
  auto initial = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  initial["value"] = static_cast<int32_t>(7);
  mailbox.open(initial);
  controller->addPV("hag-secured", mailbox, AccessAssignment{"HAGGED", 0});
  server.addSource("access", controller->source()).start();
  auto client = server.clientConfig().build();

  assert(client.get("hag-secured").exec()->wait(5.0)["value"].as<int32_t>() == 7);

  std::mutex monitorMutex;
  std::condition_variable monitorEvent;
  auto monitor = client.monitor("hag-secured")
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

  // Drive the pinned EPICS DNS cache deterministically.  The maintenance pass
  // must receive callbacks for the existing clients, clear cached rights, and
  // close the established channel so both monitor and GET become denied.
  forceHagAddress("SITE", "127.0.0.2");
  controller->pump();
  bool monitorStopped = false;
  const auto disconnectDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
  while (!monitorStopped && std::chrono::steady_clock::now() < disconnectDeadline) {
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

  bool denied = false;
  try {
    client.get("hag-secured").exec()->wait(5.0);
  } catch (const pvxs::client::RemoteError& ex) {
    denied = std::string(ex.what()).find("access denied") != std::string::npos;
  }
  assert(denied);
  assert(controller->status().rightsChanges >= 1u);

  // Restore the resolved address and let the low-frequency maintenance loop
  // run again.  The same client object must regain access without a policy
  // reload or IOC restart.
  forceHagAddress("SITE", "127.0.0.1");
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  controller->pump();
  bool restored = false;
  const auto restoreDeadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(5);
  while (!restored && std::chrono::steady_clock::now() < restoreDeadline) {
    try {
      restored = client.get("hag-secured").exec()->wait(1.0)["value"].as<int32_t>() == 7;
    } catch (const std::exception&) {
      client.hurryUp();
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  assert(restored);
  assert(controller->status().rightsChanges >= 2u);
  assert(controller->status().generation == 1u);

  server.stop();
  std::error_code ignored;
  std::filesystem::remove(policyPath, ignored);
  return 0;
}
