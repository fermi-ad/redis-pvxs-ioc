#include "redis_pvxs_ioc/app.h"
#include "redis_pvxs_ioc/config.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pvxs/client.h>
#include <pvxs/nt.h>
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/source.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 12s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(10ms);
  }
}

int main() {
  auto upstreamServer = pvxs::server::Config::isolated(AF_INET).build();
  auto source = pvxs::server::StaticSource::build();
  std::atomic<unsigned> writes{0};
  auto makePV = [&](int32_t initial) {
    auto pv = pvxs::server::SharedPV::buildMailbox();
    pv.onPut([&](pvxs::server::SharedPV& target, std::unique_ptr<pvxs::server::ExecOp>&& op,
                 pvxs::Value&& value) { ++writes; target.post(value); op->reply(); });
    auto value = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
    value["value"] = initial;
    pv.open(value);
    return pv;
  };
  auto first = makePV(10), second = makePV(20), replacement = makePV(30);
  source.add("DEVICE:first", first).add("DEVICE:second", second).add("DEVICE:replacement", replacement);
  upstreamServer.addSource("devices", source.source()).start();
  const auto upstreamPort = upstreamServer.config().tcp_port;
  assert(upstreamPort);
  auto alternateServer = pvxs::server::Config::isolated(AF_INET).build();
  auto alternate = makePV(40);
  auto alternateSource = pvxs::server::StaticSource::build();
  alternateSource.add("DEVICE:replacement", alternate);
  alternateServer.addSource("devices", alternateSource.source()).start();
  const auto alternatePort = alternateServer.config().tcp_port;
  assert(alternatePort && alternatePort != upstreamPort);

  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in local{};
  local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
  socklen_t length = sizeof(local);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&local), &length) == 0);
  const auto port = ntohs(local.sin_port);
  close(listener);

  const auto directory = std::filesystem::temp_directory_path() /
      ("redis-pvxs-gateway-app-" + std::to_string(getpid()));
  std::filesystem::create_directory(directory);
  const auto configFile = directory / "config.yaml", policyFile = directory / "policy.acf";
  const auto policy = [&](const char* permission) {
    std::ofstream out(policyFile);
    out << "ASG(DEFAULT) { RULE(0, WRITE) }\nASG(DEVICE) { RULE(0, " << permission << ") }\n";
    assert(out.good());
  };
  const auto config = [&](bool changed, bool oldEndpointLoop = false, bool alternateTarget = false) {
    std::ofstream out(configFile);
    out << "server: {instance: gateway-app, namespace: EXPORT, interfaces: [127.0.0.1], tcp_port: " << port
        << ", udp_port: " << port << ", auto_beacon: false}\n"
        << "redis: {base_key: gateway-app, host: 127.0.0.1, port: " << std::getenv("REDIS_PVXS_TEST_REDIS_PORT") << "}\n"
        << "discovery: {bind_address: 127.0.0.1, udp_port: 0}\n"
        << "access: {enabled: true, file: '" << policyFile.string() << "'}\n"
        << "pva_gateway:\n  pvs:\n";
    if (!oldEndpointLoop)
      out << "    - {name: first, upstream: 'DEVICE:" << (changed ? "replacement" : "first")
          << "', server: '127.0.0.1:" << (alternateTarget ? alternatePort : upstreamPort) << "', access: {asg: DEVICE}}\n";
    out << "    - {name: second, upstream: '" << (oldEndpointLoop ? "EXPORT:first" : "DEVICE:second")
        << "', server: '127.0.0.1:" << upstreamPort << "', access: {asg: DEVICE}}\n";
    assert(out.good());
  };
  policy("WRITE"); config(false);
  Application app(configFile.string());
  std::string error;
  assert(app.start(error));
  pvxs::client::Config clientConfig;
  clientConfig.autoAddrList = false;
  clientConfig.addressList = {"127.0.0.1:" + std::to_string(port)};
  auto client = clientConfig.build();
  const auto get = [&](const std::string& name) { return client.get(name).exec()->wait(2.0); };
  eventually([&] {
    try { return get("EXPORT:first")["value"].as<int32_t>() == 10 && get("EXPORT:second")["value"].as<int32_t>() == 20; }
    catch (const std::exception&) { return false; }
  });
  assert(get("SYS:gateway-app:stats:pvCount")["value"].as<int64_t>() == 2);
  const auto count = get("SYS:gateway-app:discovery:status")["records"].as<uint64_t>();
  ServerConfig names; names.instance = "gateway-app"; names.nameSpace = "EXPORT";
  assert(count == adminPVNames(names).size() + 2);
  const auto putDenied = [&] {
    try { client.put("EXPORT:first").set("value", int32_t(99)).exec()->wait(4.0); }
    catch (const std::exception&) { return true; }
    return false;
  };
  // Even a WRITE-granting policy cannot enable gateway writes.
  assert(putDenied() && writes.load() == 0 && first.fetch()["value"].as<int32_t>() == 10);

  std::atomic<unsigned> wakeups{0};
  auto monitor = client.monitor("EXPORT:second").maskConnected(true).maskDisconnected(false)
      .event([&](pvxs::client::Subscription&) { ++wakeups; }).exec();
  eventually([&] { try { auto value = monitor->pop(); return value && value["value"].as<int32_t>() == 20; }
                   catch (const std::exception&) { return false; } });

  // A new upstream cannot name an endpoint from the still-live generation,
  // even when the candidate also removes that endpoint.
  config(false, true); app.requestReload(); app.pump();
  assert(get("SYS:gateway-app:config:generation")["value"].as<int64_t>() == 1);
  const auto loopError = get("SYS:gateway-app:config:lastError")["value"].as<std::string>();
  assert(loopError.find("currently served endpoint") != std::string::npos);
  assert(get("EXPORT:first")["value"].as<int32_t>() == 10);

  // An invalid candidate policy cannot replace a working gateway generation.
  config(true); policy("NOT_A_PERMISSION"); app.requestReload(); app.pump();
  assert(get("SYS:gateway-app:config:generation")["value"].as<int64_t>() == 1);
  assert(get("EXPORT:first")["value"].as<int32_t>() == 10);
  assert(get("SYS:gateway-app:config:reloadStatus")["state"].as<std::string>() == "rejected");

  policy("WRITE"); app.requestReload(); app.pump();
  assert(get("SYS:gateway-app:config:generation")["value"].as<int64_t>() == 2);
  eventually([&] { try { return get("EXPORT:first")["value"].as<int32_t>() == 30; }
                   catch (const std::exception&) { return false; } });
  assert(putDenied() && writes.load() == 0 && replacement.fetch()["value"].as<int32_t>() == 30);
  auto next = second.fetch(); next["value"] = int32_t(21); second.post(next);
  bool disconnected = false;
  eventually([&] {
    try { auto value = monitor->pop(); return value && value["value"].as<int32_t>() == 21; }
    catch (const pvxs::client::Disconnect&) { disconnected = true; return false; }
    catch (const std::exception&) { return false; }
  });
  assert(!disconnected); // unchanged gateway retains its channel across reload
  auto status = get("SYS:gateway-app:config:reloadStatus");
  assert(status["recreated"].as<uint64_t>() == 1 && status["retained"].as<uint64_t>() == 1);
  eventually([&] {
    app.pump();
    return get("SYS:gateway-app:discovery:status")["desiredGeneration"].as<uint64_t>() == 2;
  });

  // Changing only the selected TCP server must recreate just that mapping.
  config(true, false, true); app.requestReload(); app.pump();
  assert(get("SYS:gateway-app:config:generation")["value"].as<int64_t>() == 3);
  eventually([&] { try { return get("EXPORT:first")["value"].as<int32_t>() == 40; }
                   catch (const std::exception&) { return false; } });
  assert(putDenied() && writes.load() == 0 && alternate.fetch()["value"].as<int32_t>() == 40);
  status = get("SYS:gateway-app:config:reloadStatus");
  assert(status["recreated"].as<uint64_t>() == 1 && status["retained"].as<uint64_t>() == 1);

  // The gateway assignment participates in normal ACF policy reloads.
  policy("READ"); app.requestReload(); app.pump();
  assert(get("SYS:gateway-app:config:generation")["value"].as<int64_t>() == 4);
  assert(putDenied() && writes.load() == 0 && alternate.fetch()["value"].as<int32_t>() == 40);
  assert(get("EXPORT:first")["value"].as<int32_t>() == 40);
  monitor->cancel();
  client.close(); app.stop();

  // The full application also rejects executed PUTs when ACF is disabled.
  const int plainListener = socket(AF_INET, SOCK_STREAM, 0);
  assert(plainListener >= 0);
  sockaddr_in plainLocal{};
  plainLocal.sin_family = AF_INET; plainLocal.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(plainListener, reinterpret_cast<sockaddr*>(&plainLocal), sizeof(plainLocal)) == 0);
  socklen_t plainLength = sizeof(plainLocal);
  assert(getsockname(plainListener, reinterpret_cast<sockaddr*>(&plainLocal), &plainLength) == 0);
  const auto plainPort = ntohs(plainLocal.sin_port);
  close(plainListener);
  {
    std::ofstream out(configFile);
    out << "server: {instance: gateway-plain, namespace: PLAIN, interfaces: [127.0.0.1], tcp_port: "
        << plainPort << ", udp_port: " << plainPort << ", auto_beacon: false}\n"
        << "redis: {base_key: gateway-plain, host: 127.0.0.1, port: "
        << std::getenv("REDIS_PVXS_TEST_REDIS_PORT") << "}\n"
        << "discovery: {enabled: false}\n"
        << "pva_gateway: {pvs: [{name: first, upstream: 'DEVICE:first', server: '127.0.0.1:"
        << upstreamPort << "'}]}\n";
    assert(out.good());
  }
  Application plainApp(configFile.string());
  assert(plainApp.start(error));
  pvxs::client::Config plainClientConfig;
  plainClientConfig.autoAddrList = false;
  plainClientConfig.addressList = {"127.0.0.1:" + std::to_string(plainPort)};
  auto plainClient = plainClientConfig.build();
  eventually([&] {
    try { return plainClient.get("PLAIN:first").exec()->wait(2.0)["value"].as<int32_t>() == 10; }
    catch (const std::exception&) { return false; }
  });
  bool plainDenied = false;
  try { plainClient.put("PLAIN:first").set("value", int32_t(99)).exec()->wait(4.0); }
  catch (const pvxs::client::RemoteError& ex) {
    plainDenied = std::string(ex.what()).find("Read-only PV") != std::string::npos;
  }
  assert(plainDenied && writes.load() == 0 && first.fetch()["value"].as<int32_t>() == 10);
  assert(plainClient.get("PLAIN:first").exec()->wait(2.0)["value"].as<int32_t>() == 10);
  plainClient.close(); plainApp.stop();
  upstreamServer.stop(); alternateServer.stop();
  std::filesystem::remove_all(directory);
}
