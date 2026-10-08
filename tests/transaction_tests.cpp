#include "redis_pvxs_ioc/app.h"
#include "RedisAdapter.hpp"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pvxs/client.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) { assert(std::chrono::steady_clock::now() < deadline); std::this_thread::sleep_for(2ms); }
}

int main() {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in local{};
  local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
  socklen_t size = sizeof(local);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size) == 0);
  const auto port = ntohs(local.sin_port);
  close(listener);
  const auto directory = std::filesystem::temp_directory_path() / ("redis-pvxs-transaction-" + std::to_string(getpid()));
  std::filesystem::create_directory(directory);
  const auto configFile = directory / "config.yaml", policyFile = directory / "policy.acf";
  const auto writePolicy = [&](bool valid) {
    std::ofstream output(policyFile);
    output << (valid ? "ASG(DEFAULT) { RULE(0, WRITE) }\n" : "ASG(DEFAULT) { RULE(0, NOT_A_PERMISSION) }\n");
    assert(output.good());
  };
  const auto writeConfig = [&](bool changed) {
    std::ofstream output(configFile);
    output << "server: {instance: transaction, namespace: TEST, interfaces: [127.0.0.1], tcp_port: " << port
           << ", udp_port: " << port << ", auto_beacon: false}\n"
           << "redis: {base_key: transaction, host: 127.0.0.1, port: " << std::getenv("REDIS_PVXS_TEST_REDIS_PORT") << "}\n"
           << "discovery: {bind_address: 127.0.0.1, udp_port: 0}\n"
           << "access: {enabled: true, file: '" << policyFile.string() << "'}\n"
           << "pvs:\n  - name: keep\n    type: float64\n    shape: scalar\n    aliases: [TEST:"
           << (changed ? "new" : "old") << "]\n    read: {key: read}\n    write: {key: command}\n"
           << "    confirm: {key: ack, timeout_ms: 5000}\n    metadata: {description: " << (changed ? "new" : "old") << "}\n";
    if (changed) output << "    transform: {scale: 2, offset: 0}\n";
    output << "  - name: " << (changed ? "added" : "removed")
           << "\n    type: float64\n    shape: scalar\n    read: {key: other}\n";
    assert(output.good());
  };
  writePolicy(true); writeConfig(false);
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  RedisAdapter producer("transaction", options);
  assert(producer.addSingleDouble("read", 4.).ok());
  assert(producer.addSingleDouble("other", 9.).ok());
  Application app(configFile.string());
  std::string error;
  assert(app.start(error));
  pvxs::client::Config clientConfig;
  clientConfig.addressList = {"127.0.0.1:" + std::to_string(port)};
  clientConfig.autoAddrList = false;
  auto client = clientConfig.build();
  const auto get = [&](const std::string& name) { return client.get(name).exec()->wait(2.); };
  const auto report = [&] { return get("SYS:transaction:config:reloadStatus"); };
  const auto onlyBackend = [&](const pvxs::Value& value, const std::string& field) {
    const auto result = value["backends." + field].as<pvxs::shared_array<const std::string>>();
    assert(result.size() == 1);
    return result[0];
  };
  assert(get("TEST:old")["value"].as<double>() == 4.);
  auto status = report();
  assert(status["kind"].as<std::string>() == "startup" && status["committed"].as<bool>());
  assert(status["attempt"].as<uint64_t>() == 1 && status["schemaVersion"].as<uint64_t>() == 1);
  assert(status["added"].as<uint64_t>() == 2 && status["retained"].as<uint64_t>() == 0);
  assert(status["durationMs"].as<double>() >= 0);
  assert(onlyBackend(status, "action") == "created" && onlyBackend(status, "preconnect") == "connected");
  assert(onlyBackend(status, "cutover") == "active");
  eventually([&] {
    app.pump();
    return get("SYS:transaction:alarms:status")["state"].as<std::string>() == "ready";
  });
  assert(get("SYS:transaction:alarms:status")["active"].as<uint64_t>() == 2);
  const auto fingerprint = get("SYS:transaction:access:policyFingerprint")["value"].as<std::string>();
  std::mutex mutex;
  std::condition_variable changed;
  auto monitor = client.monitor("TEST:keep").maskConnected(true).maskDisconnected(false)
      .event([&](pvxs::client::Subscription&) { changed.notify_all(); }).exec();
  const auto observe = [&](double expected) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    for (;;) {
      if (const auto value = monitor->pop(); value && value["value"].as<double>() == expected) return;
      std::unique_lock<std::mutex> lock(mutex);
      assert(std::chrono::steady_clock::now() < deadline);
      changed.wait_for(lock, 5ms);
    }
  };
  observe(4.);
  bool completed = false;
  std::string putError;
  auto pending = client.put("TEST:old").set("value", 100.).result([&](pvxs::client::Result&& result) {
    std::string failure;
    try { result(); } catch (const std::exception& ex) { failure = ex.what(); }
    std::lock_guard<std::mutex> guard(mutex);
    completed = true; putError = failure; changed.notify_all();
  }).exec();
  eventually([&] { return producer.getStreamSnapshot("command").present(); });
  writeConfig(true); writePolicy(false);
  app.requestReload(); app.pump();
  assert(get("SYS:transaction:config:generation")["value"].as<int64_t>() == 1);
  status = report();
  assert(status["attempt"].as<uint64_t>() == 2 && status["state"].as<std::string>() == "rejected");
  assert(status["phase"].as<std::string>() == "policy" && !status["committed"].as<bool>());
  assert(status["activeGeneration"].as<uint64_t>() == 1 && status["candidateGeneration"].as<uint64_t>() == 2);
  assert(status["diffKnown"].as<bool>() && !status["error"].as<std::string>().empty());
  assert(status["added"].as<uint64_t>() == 1 && status["removed"].as<uint64_t>() == 1);
  assert(status["recreated"].as<uint64_t>() == 0 && status["retained"].as<uint64_t>() == 1);
  assert(status["metadataChanged"].as<uint64_t>() == 1 && status["aliasesChanged"].as<uint64_t>() == 1);
  assert(onlyBackend(status, "action") == "retained" && onlyBackend(status, "cutover") == "previous-preserved");
  assert(get("SYS:transaction:access:policyFingerprint")["value"].as<std::string>() == fingerprint);
  assert(get("TEST:keep")["display.description"].as<std::string>() == "old");
  assert(get("TEST:old")["value"].as<double>() == 4.);
  assert(get("TEST:removed")["value"].as<double>() == 9.);
  assert(get("SYS:transaction:discovery:status")["desiredGeneration"].as<uint64_t>() == 1);
  {
    std::lock_guard<std::mutex> guard(mutex); assert(!completed);
  }
  assert(producer.addSingleDouble("ack", 100.).ok());
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 2s, [&] { return completed; }));
    assert(putError.empty());
  }
  assert(producer.addSingleDouble("read", 5.).ok()); observe(5.);

  writePolicy(true);
  app.requestReload(); app.pump();
  assert(get("SYS:transaction:config:generation")["value"].as<int64_t>() == 2);
  status = report();
  assert(status["attempt"].as<uint64_t>() == 3 && status["state"].as<std::string>() == "committed");
  assert(status["activeGeneration"].as<uint64_t>() == 2 && status["candidateGeneration"].as<uint64_t>() == 2);
  assert(status["committed"].as<bool>() && status["error"].as<std::string>().empty());
  assert(onlyBackend(status, "cutover") == "active");
  observe(10.); // canonical monitor survives alias removal and metadata cutover
  assert(get("TEST:new")["value"].as<double>() == 10.);
  assert(get("TEST:added")["value"].as<double>() == 9.);
  assert(get("TEST:keep")["display.description"].as<std::string>() == "new");
  assert(producer.addSingleDouble("read", 6.).ok()); observe(12.);
  {
    std::ofstream output(configFile); output << "server: [ invalid\n";
  }
  app.requestReload(); app.pump();
  status = report();
  assert(status["attempt"].as<uint64_t>() == 4 && status["state"].as<std::string>() == "failed");
  assert(!status["diffKnown"].as<bool>() && status["phase"].as<std::string>() == "parse");
  assert(status["activeGeneration"].as<uint64_t>() == 2 && !status["committed"].as<bool>());
  assert(status["added"].as<uint64_t>() == 0 && status["metadataChanged"].as<uint64_t>() == 0);
  assert(status["backends.name"].as<pvxs::shared_array<const std::string>>().empty());
  assert(get("SYS:transaction:config:lastDiff")["value"].as<std::string>() == "{}");
  assert(get("TEST:keep")["value"].as<double>() == 12.);
  writeConfig(true);
  app.requestReload(); app.pump();
  status = report();
  assert(status["attempt"].as<uint64_t>() == 5 && status["activeGeneration"].as<uint64_t>() == 3);
  assert(status["state"].as<std::string>() == "committed" && status["error"].as<std::string>().empty());
  assert(status["added"].as<uint64_t>() == 0 && status["retained"].as<uint64_t>() == 2);
  assert(status["recreated"].as<uint64_t>() == 0 && status["metadataChanged"].as<uint64_t>() == 0);
  monitor.reset(); client.close(); app.stop();
  std::filesystem::remove(configFile); std::filesystem::remove(policyFile); std::filesystem::remove(directory);
}
