#include "redis_pvxs_ioc/app.h"
#include "RedisAdapter.hpp"
#include <alarm.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pvxs/client.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventuallyImpl(F predicate, const char* expression) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "timed out: " << expression << '\n'; assert(false);
    }
    std::this_thread::sleep_for(10ms);
  }
}
#define eventually(...) eventuallyImpl(__VA_ARGS__, #__VA_ARGS__)
struct Completion {
  std::mutex mutex;
  bool done = false;
  std::string error;
  void complete(pvxs::client::Result&& result) {
    std::string failure;
    try { result(); } catch (const std::exception& e) { failure = e.what(); }
    std::lock_guard<std::mutex> lock(mutex); error = failure; done = true;
  }
  bool finished() { std::lock_guard<std::mutex> lock(mutex); return done; }
};

int main() {
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in local{}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
  socklen_t size = sizeof(local);
  assert(getsockname(listener, reinterpret_cast<sockaddr*>(&local), &size) == 0);
  const auto port = ntohs(local.sin_port); close(listener);
  const auto directory = std::filesystem::temp_directory_path() / ("redis-pvxs-health-" + std::to_string(getpid()));
  std::filesystem::create_directory(directory);
  const auto path = directory / "config.yaml";
  const auto writeConfig = [&](bool required) {
    std::ofstream output(path);
    output << "server: {instance: health-pva, namespace: TEST, interfaces: [127.0.0.1], tcp_port: " << port
           << ", udp_port: " << port << ", auto_beacon: false}\n"
           << "redis_backends:\n  first: {base_key: health-pva, host: 127.0.0.1, port: "
           << std::getenv("REDIS_PVXS_TEST_REDIS_PORT") << ", reader_probe_ms: 100}\n"
           << "  second: {base_key: health-pva, host: 127.0.0.1, port: "
           << std::getenv("REDIS_PVXS_SECOND_TEST_PORT") << ", reader_probe_ms: 100}\n"
           << "alarms: {backend: first, stream: health-pva-alarms}\ndiscovery: {enabled: false}\n"
           << "pvs:\n  - name: first\n    aliases: [TEST:alias]\n    type: float64\n    shape: scalar\n"
           << "    read: {backend: first, key: first}\n    write: {backend: first, key: command}\n"
           << "    confirm: {backend: first, key: first, timeout_ms: 5000}\n"
           << "  - name: second\n    type: float64\n    shape: scalar\n    read: {backend: second, key: second}\n"
           << "  - name: waveform\n    type: float32\n    shape: array\n    read: {backend: first, key: waveform}\n"
           << "    metadata: {units: mm}\n"
           << "    source_time: {mode: unix-nanoseconds-v1, field: sample_time_ns, valid_field: sample_time_valid}\n"
           << "  - name: optional\n    type: float64\n    shape: scalar\n    read: {backend: first, key: optional}\n"
           << "    source_health: {required: " << (required ? "true" : "false") << "}\n";
    assert(output.good());
  };
  RA_Options options; options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  RedisAdapter first("health-pva", options);
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_SECOND_TEST_PORT"));
  RedisAdapter second("health-pva", options);
  assert(first.addSingleDouble("first", 4).ok());
  assert(second.addSingleDouble("second", 8).ok());
  const float waveform[] = {1.25f, -2.5f};
  std::string waveformBytes(sizeof(waveform), '\0');
  std::memcpy(waveformBytes.data(), waveform, sizeof(waveform));
  RedisAdapter::Attrs waveformData{{"_", waveformBytes}};
  assert(first.addSingleValue<RedisAdapter::Attrs>("waveform", waveformData).ok());
  writeConfig(false);
  const auto secondPid = static_cast<pid_t>(std::stoi(std::getenv("REDIS_PVXS_SECOND_TEST_PID")));
  assert(kill(secondPid, SIGSTOP) == 0);
  Application app(path.string()); std::string error; assert(app.start(error));
  pvxs::client::Config clientConfig;
  clientConfig.addressList = {"127.0.0.1:" + std::to_string(port)}; clientConfig.autoAddrList = false;
  auto client = clientConfig.build();
  const auto get = [&](const std::string& name) { return client.get(name).exec()->wait(2.); };
  const auto report = [&] { app.pump(); return get("SYS:health-pva:source:status"); };
  const auto row = [](const pvxs::Value& value, const std::string& name) {
    const auto names = value["sources.pv"].as<pvxs::shared_array<const std::string>>();
    for (size_t i = 0; i < names.size(); ++i) if (names[i] == name) return i;
    assert(false); return size_t(0);
  };
  const auto number = [&](const pvxs::Value& value, const std::string& name, const std::string& field) {
    return value["sources." + field].as<pvxs::shared_array<const uint64_t>>()[row(value, name)];
  };
  const auto text = [&](const pvxs::Value& value, const std::string& name, const std::string& field) {
    return value["sources." + field].as<pvxs::shared_array<const std::string>>()[row(value, name)];
  };
  const auto flag = [&](const pvxs::Value& value, const std::string& name, const std::string& field) {
    return value["sources." + field].as<pvxs::shared_array<const bool>>()[row(value, name)];
  };
  eventually([&] { return text(report(), "TEST:first", "state") == "ready"; });
  assert(!report()["ready"].as<bool>());
  assert(get("TEST:alias")["value"].as<double>() == 4);
  assert(get("TEST:second")["value"].as<double>() == 0);
  assert(get("TEST:second")["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);
  assert(text(report(), "TEST:first", "sourceTimeMode") == "legacy-redis-id");
  assert(flag(report(), "TEST:first", "sourceTimePresent") && flag(report(), "TEST:first", "sourceTimeValid"));
  assert(!flag(report(), "TEST:second", "sourceTimePresent"));
  assert(kill(secondPid, SIGCONT) == 0);
  eventually([&] { return !text(report(), "TEST:second", "cursor").empty(); });
  // An unavailable initial snapshot cannot replay a retained tail as valid
  // source data. A new observation establishes validity after reconnection.
  assert(second.addSingleDouble("second", 8).ok());
  eventually([&] { return report()["ready"].as<bool>(); });
  auto before = report();
  assert(before["total"].as<uint64_t>() == 4 && before["required"].as<uint64_t>() == 3);
  assert(before["readySources"].as<uint64_t>() == 3);
  assert(get("SYS:health-pva:ready")["value"].as<bool>());
  assert(text(before, "TEST:first", "role") == "read-confirm");
  assert(get("TEST:alias")["value"].as<double>() == 4);

  // Required payload readiness is independent of timestamp quality, and the
  // PVA diagnostic arrays distinguish unknown from present-but-invalid time.
  assert(flag(before, "TEST:waveform", "ready") && flag(before, "TEST:waveform", "valid"));
  assert(text(before, "TEST:waveform", "sourceTimeMode") == "unix-nanoseconds-v1");
  assert(!flag(before, "TEST:waveform", "sourceTimePresent") && !flag(before, "TEST:waveform", "sourceTimeValid"));
  assert(!text(before, "TEST:waveform", "sourceTimeError").empty());
  auto wave = get("TEST:waveform");
  const auto samples = wave["value"].as<pvxs::shared_array<const float>>();
  assert(samples.size() == 2 && std::memcmp(samples.data(), waveform, sizeof(waveform)) == 0);
  assert(wave["display.units"].as<std::string>() == "mm");
  assert(wave["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(wave["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);
  waveformData["sample_time_ns"] = "1700000000123456789"; waveformData["sample_time_valid"] = "0";
  assert(first.addSingleValue<RedisAdapter::Attrs>("waveform", waveformData).ok());
  eventually([&] { return flag(report(), "TEST:waveform", "sourceTimePresent"); });
  auto invalidTime = report();
  assert(invalidTime["ready"].as<bool>() && flag(invalidTime, "TEST:waveform", "ready"));
  assert(!flag(invalidTime, "TEST:waveform", "sourceTimeValid"));
  wave = get("TEST:waveform");
  assert(wave["timeStamp.secondsPastEpoch"].as<int64_t>() == 1700000000);
  assert(wave["timeStamp.nanoseconds"].as<int32_t>() == 123456789);
  assert(wave["alarm.severity"].as<int>() == epicsSevInvalid);
  waveformData["sample_time_valid"] = "1";
  assert(first.addSingleValue<RedisAdapter::Attrs>("waveform", waveformData).ok());
  eventually([&] { return flag(report(), "TEST:waveform", "sourceTimeValid"); });
  assert(text(report(), "TEST:waveform", "sourceTimeError").empty());
  assert(get("TEST:waveform")["alarm.severity"].as<int>() == epicsSevNone);

  // A rejected config does not replace the status generation or retained source.
  { std::ofstream output(path); output << "unknown: true\n"; }
  app.requestReload(); app.pump();
  assert(report()["generation"].as<uint64_t>() == 1);
  assert(text(report(), "TEST:first", "cursor") == text(before, "TEST:first", "cursor"));
  writeConfig(true); app.requestReload(); app.pump();
  auto changed = report();
  assert(changed["generation"].as<uint64_t>() == 2 && !changed["ready"].as<bool>());
  assert(changed["unreadyRequired"].as<uint64_t>() == 1);
  assert(text(changed, "TEST:first", "cursor") == text(before, "TEST:first", "cursor"));
  assert(number(changed, "TEST:first", "epoch") == number(before, "TEST:first", "epoch"));
  assert(get("TEST:alias")["value"].as<double>() == 4);
  writeConfig(false); app.requestReload(); app.pump();
  eventually([&] { return report()["ready"].as<bool>(); });

  // The fixture owns this second Redis PID. Pausing it must not block the first
  // backend's values, alias or diagnostics; the wrapper resumes it on failures.
  assert(kill(secondPid, SIGSTOP) == 0);
  eventually([&] { return text(report(), "TEST:second", "state") == "disconnected"; });
  const auto paused = report();
  assert(!paused["ready"].as<bool>() && number(paused, "TEST:second", "readFailures") > 0);
  assert(get("TEST:second")["value"].as<double>() == 8);
  const auto firstTime = first.addSingleDouble("first", 5); assert(firstTime.ok());
  eventually([&] { return get("TEST:alias")["value"].as<double>() == 5; });
  assert(text(report(), "TEST:first", "state") == "ready");
  assert(kill(secondPid, SIGCONT) == 0);
  eventually([&] { return report()["ready"].as<bool>(); });
  const auto recovered = report();
  assert(number(recovered, "TEST:second", "reconnects") > 0);
  assert(number(recovered, "TEST:second", "epoch") == number(before, "TEST:second", "epoch"));
  assert(text(recovered, "TEST:second", "cursor") == text(before, "TEST:second", "cursor"));

  // A pending confirmation cannot cross a deleted/recreated source epoch.
  Completion completion;
  auto put = client.put("TEST:alias").set("value", 123.)
      .result([&](pvxs::client::Result&& result) { completion.complete(std::move(result)); }).exec();
  eventually([&] { return first.getStreamSnapshot("command").present(); });
  sw::redis::ConnectionOptions controlOptions; controlOptions.host = "127.0.0.1";
  controlOptions.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  sw::redis::Redis control(controlOptions); control.del("{health-pva}:first");
  eventually([&] { app.pump(); return completion.finished(); });
  assert(completion.error.find("source epoch changed") != std::string::npos);
  assert(get("TEST:alias")["value"].as<double>() == 5);
  assert(get("TEST:first")["timeStamp.nanoseconds"].as<int32_t>() == firstTime.value % 1000000000);
  assert(!report()["ready"].as<bool>());
  assert(first.addSingleDouble("first", 6, {RA_Time("1-0")}).ok());
  eventually([&] { return report()["ready"].as<bool>(); });
  const auto replacement = report();
  assert(number(replacement, "TEST:first", "epoch") == 2);
  assert(text(replacement, "TEST:first", "cursor") == "1-0");
  assert(get("TEST:alias")["value"].as<double>() == 6);
  app.stop();
  std::filesystem::remove_all(directory);
  std::cout << "PVA source health: partial outage, recovery, aliases, rejected/retained reloads and confirmation epochs passed\n";
}
