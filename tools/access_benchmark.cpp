#include "redis_pvxs_ioc/access_control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <thread>
#include <vector>

#include <pvxs/client.h>
#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;

namespace {

using Clock = std::chrono::steady_clock;

struct Measurements {
  std::vector<double> getUs;
  std::vector<double> putUs;
  double monitorPerSecond = 0.0;
  double wallSeconds = 0.0;
  double cpuSeconds = 0.0;
  long maxRssKb = 0;
  uint64_t denied = 0;
  uint64_t reconnectDisruptions = 0;
};

double quantile(std::vector<double> values, const double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>((values.size() - 1u) * fraction);
  return values[index];
}

double elapsedUs(const Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double usageSeconds(const rusage& usage) {
  return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
         usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}

void writePolicy(const std::filesystem::path& path, const std::string& policy) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << policy;
  if (!output.good()) throw std::runtime_error("unable to write benchmark policy");
}

void printSeries(std::ostream& out, const std::string& name, const std::vector<double>& values) {
  const auto seconds = std::accumulate(values.begin(), values.end(), 0.0) / 1e6;
  out << "    \"" << name << "_rate\": " << (seconds > 0.0 ? values.size() / seconds : 0.0) << ",\n"
      << "    \"" << name << "_p50_us\": " << quantile(values, 0.50) << ",\n"
      << "    \"" << name << "_p95_us\": " << quantile(values, 0.95) << ",\n"
      << "    \"" << name << "_p99_us\": " << quantile(values, 0.99) << ",\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "baseline";
  size_t iterations = 10000u;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--mode" && index + 1 < argc) mode = argv[++index];
    else if (arg == "--iterations" && index + 1 < argc) iterations = std::stoull(argv[++index]);
    else {
      std::cerr << "usage: access_benchmark --mode baseline|allow|mixed|reload [--iterations N]\n";
      return 2;
    }
  }
  if (mode != "baseline" && mode != "allow" && mode != "mixed" && mode != "reload") return 2;

  try {
    auto server = pvxs::server::Config::isolated(AF_INET).build();
    auto mailbox = pvxs::server::SharedPV::buildMailbox();
    auto initial = pvxs::nt::NTScalar{pvxs::TypeCode::Int64}.create();
    initial["value"] = static_cast<int64_t>(0);
    mailbox.onPut([](pvxs::server::SharedPV& pv,
                     std::unique_ptr<pvxs::server::ExecOp>&& op,
                     pvxs::Value&& value) {
      pv.post(value);
      op->reply();
    });
    mailbox.open(initial);

    std::shared_ptr<AccessController> access;
    std::filesystem::path policyPath;
    if (mode == "baseline") {
      server.addPV("bench", mailbox);
    } else {
      policyPath = std::filesystem::temp_directory_path() / "redis-pvxs-ioc-benchmark.acf";
      writePolicy(policyPath,
                  "ASG(ALLOW) { RULE(0, WRITE) }\n"
                  "ASG(DENY) { RULE(0, NONE) }\n");
      AccessConfig config;
      config.enabled = true;
      config.file = policyPath.string();
      access = std::make_shared<AccessController>(config);
      std::string error;
      if (!access->start({"ALLOW", "DENY"}, error)) throw std::runtime_error(error);
      access->addPV("bench", mailbox, AccessAssignment{"ALLOW", 0});
      if (mode == "mixed") access->addPV("bench-denied", mailbox, AccessAssignment{"DENY", 0});
      server.addSource("access", access->source());
    }

    server.start();
    auto client = server.clientConfig().build();
    const auto warmupIterations = std::min<size_t>(1000u, std::max<size_t>(1u, iterations / 10u));
    for (size_t index = 0; index < warmupIterations; ++index) {
      client.get("bench").exec()->wait(5.0);
    }
    if (mode != "mixed") {
      for (size_t index = 0; index < warmupIterations; ++index) {
        client.put("bench").set("value", static_cast<int64_t>(index)).exec()->wait(5.0);
      }
    }

    Measurements measured;
    measured.getUs.reserve(iterations);
    measured.putUs.reserve(iterations);
    rusage before{};
    rusage after{};
    getrusage(RUSAGE_SELF, &before);
    const auto wallStart = Clock::now();

    for (size_t index = 0; index < iterations; ++index) {
      const auto start = Clock::now();
      const auto name = mode == "mixed" && (index % 2u) ? "bench-denied" : "bench";
      try {
        client.get(name).exec()->wait(5.0);
      } catch (const pvxs::client::RemoteError&) {
        ++measured.denied;
      }
      measured.getUs.push_back(elapsedUs(start));
    }

    if (mode != "mixed") {
      for (size_t index = 0; index < iterations; ++index) {
        const auto start = Clock::now();
        client.put("bench").set("value", static_cast<int64_t>(index)).exec()->wait(5.0);
        measured.putUs.push_back(elapsedUs(start));
      }
    }

    std::mutex monitorMutex;
    std::condition_variable monitorChanged;
    std::atomic<size_t> monitorCount{0u};
    auto subscription = client.monitor("bench")
        .record("queueSize", static_cast<uint32_t>(iterations + 8u))
        .record("pipeline", true)
        .maskConnected(true)
        .maskDisconnected(true)
        .event([&](pvxs::client::Subscription& sub) {
          while (sub.pop()) monitorCount.fetch_add(1u, std::memory_order_relaxed);
          monitorChanged.notify_all();
        }).exec();
    client.hurryUp();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    monitorCount.store(0u, std::memory_order_relaxed);
    const auto monitorStart = Clock::now();
    for (size_t index = 0; index < iterations; ++index) {
      auto update = initial.cloneEmpty();
      update["value"] = static_cast<int64_t>(index);
      mailbox.post(update);
    }
    {
      std::unique_lock<std::mutex> guard(monitorMutex);
      monitorChanged.wait_for(guard, std::chrono::seconds(10), [&]() {
        return monitorCount.load(std::memory_order_relaxed) >= iterations;
      });
    }
    const auto monitorSeconds = std::chrono::duration<double>(Clock::now() - monitorStart).count();
    measured.monitorPerSecond = monitorSeconds > 0.0 ? monitorCount.load() / monitorSeconds : 0.0;

    if (mode == "reload") {
      std::atomic<bool> keepLoading{true};
      std::atomic<uint64_t> disruptions{0u};
      std::thread load([&]() {
        while (keepLoading.load(std::memory_order_relaxed)) {
          try {
            client.get("bench").exec()->wait(5.0);
          } catch (const std::exception&) {
            disruptions.fetch_add(1u, std::memory_order_relaxed);
          }
        }
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      writePolicy(policyPath, "ASG(ALLOW) { RULE(0, NONE) }\nASG(DENY) { RULE(0, NONE) }\n");
      std::string error;
      if (!access->reload("benchmark-deny", error)) {
        keepLoading.store(false, std::memory_order_relaxed);
        load.join();
        throw std::runtime_error(error);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      writePolicy(policyPath, "ASG(ALLOW) { RULE(0, WRITE) }\nASG(DENY) { RULE(0, NONE) }\n");
      if (!access->reload("benchmark-restore", error)) {
        keepLoading.store(false, std::memory_order_relaxed);
        load.join();
        throw std::runtime_error(error);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      keepLoading.store(false, std::memory_order_relaxed);
      load.join();
      measured.reconnectDisruptions = disruptions.load(std::memory_order_relaxed);
      client.get("bench").exec()->wait(5.0);
    }

    measured.wallSeconds = std::chrono::duration<double>(Clock::now() - wallStart).count();
    getrusage(RUSAGE_SELF, &after);
    measured.cpuSeconds = usageSeconds(after) - usageSeconds(before);
#ifdef __APPLE__
    measured.maxRssKb = after.ru_maxrss / 1024;
#else
    measured.maxRssKb = after.ru_maxrss;
#endif

    std::cout << std::fixed << std::setprecision(3)
              << "{\n  \"mode\": \"" << mode << "\",\n"
              << "  \"iterations\": " << iterations << ",\n"
              << "  \"metrics\": {\n";
    printSeries(std::cout, "get", measured.getUs);
    printSeries(std::cout, "put", measured.putUs);
    std::cout << "    \"monitor_rate\": " << measured.monitorPerSecond << ",\n"
              << "    \"cpu_seconds\": " << measured.cpuSeconds << ",\n"
              << "    \"wall_seconds\": " << measured.wallSeconds << ",\n"
              << "    \"max_rss_kb\": " << measured.maxRssKb << ",\n"
              << "    \"denied_operations\": " << measured.denied << ",\n"
              << "    \"reconnect_disruptions\": " << measured.reconnectDisruptions << "\n"
              << "  }\n}\n";

    server.stop();
    if (!policyPath.empty()) {
      std::error_code ignored;
      std::filesystem::remove(policyPath, ignored);
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "access benchmark failed: " << ex.what() << '\n';
    return 1;
  }
}
