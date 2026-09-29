// Bounded Redis -> external IOC -> PVA measurement. Fan-out uses independent
// client contexts monitoring one canonical PV from a private Redis stream.
#include "RedisAdapter.hpp"
#include <pvxs/client.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/resource.h>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
uint32_t number(const std::string& value, uint32_t maximum) {
  uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result && result <= maximum,
          "numeric option is outside its bounds");
  return result;
}

int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1", base, pv, address, output;
    uint32_t port = 0, samples = 1000, rate = 100, elements = 1, count = 1;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      require(i + 1 < argc, "missing option value");
      const std::string value = argv[++i];
      if (option == "--redis-host") host = value;
      else if (option == "--redis-port") port = number(value, 65535);
      else if (option == "--base-key") base = value;
      else if (option == "--pv") pv = value;
      else if (option == "--pva-address") address = value;
      else if (option == "--output") output = value;
      else if (option == "--samples") samples = number(value, 100000);
      else if (option == "--rate") rate = number(value, 10000);
      else if (option == "--elements") elements = number(value, 8388608);
      else if (option == "--clients") count = number(value, 64);
      else throw std::runtime_error("unknown option: " + option);
    }
    require(port && !base.empty() && !pv.empty() && !address.empty() && !output.empty(),
            "required: --redis-port --base-key --pv --pva-address --output");
    require(samples / double(rate) <= 600 && uint64_t(samples) * count <= 1000000,
            "measurement exceeds duration or sample memory bound");
    require(!std::filesystem::exists(output), "report exists; choose a new evidence path");
    RA_Options options; options.cxn.host = host; options.cxn.port = port;
    RedisAdapter producer(base, options);
    require(producer.connected(), "private Redis unavailable");
    require(!producer.getStreamSnapshot("data").present(), "stream exists; use an isolated base key");
    pvxs::client::Config config; config.autoAddrList = false; config.addressList = {address};
    config.udp_port = 0; // independent search listeners for clients sharing one test host
    std::vector<pvxs::client::Context> clients;
    std::mutex mutex;
    std::condition_variable changed;
    std::string failure;
    struct Observations {
      bool initial = false;
      uint32_t last = 0, received = 0, duplicates = 0;
      std::vector<double> latency;
      pvxs::client::SubscriptionStat queue;
    };
    std::vector<Observations> observations(count);
    std::vector<Clock::time_point> sent(samples + 1);
    std::vector<std::shared_ptr<pvxs::client::Subscription>> monitors;
    struct CancelAll {
      decltype(monitors)& subscriptions;
      ~CancelAll() { for (auto& subscription : subscriptions) subscription->cancel(); }
    } cleanup{monitors}; // joins callbacks before their captured state is destroyed
    for (uint32_t index = 0; index < count; ++index) {
      observations[index].latency.reserve(samples);
      clients.push_back(config.build());
      monitors.push_back(clients.back().monitor(pv)
          .record("queueSize", uint32_t(16)).record("pipeline", true)
          .maskConnected(true).maskDisconnected(false).event([&, index](pvxs::client::Subscription& sub) {
        try {
          while (const auto value = sub.pop()) {
            const auto arrived = Clock::now();
            uint32_t id;
            if (elements == 1) {
              require(value.idStartsWith("epics:nt/NTScalar:1.0"), "wrong scalar type");
              id = value["value"].as<uint32_t>();
            } else {
              require(value.idStartsWith("epics:nt/NTScalarArray:1.0"), "wrong array type");
              const auto data = value["value"].as<pvxs::shared_array<const uint32_t>>();
              if (data.empty()) continue; // initial empty value before first Redis sample
              require(data.size() == elements, "wrong array length");
              id = data[0];
              for (size_t i = 0; i < data.size(); ++i) require(data[i] == id + i, "array corruption");
            }
            std::lock_guard<std::mutex> guard(mutex);
            auto& observed = observations[index];
            if (id == 0) {
              require(!observed.last, "initial sample arrived after data");
              observed.initial = true; changed.notify_all(); continue;
            }
            require(id <= samples && id >= observed.last && sent[id] != Clock::time_point{}, "invalid sample order/identity");
            require(value["alarm.severity"].as<int32_t>() == 0, "sample has invalid alarm");
            if (id == observed.last) { ++observed.duplicates; continue; }
            observed.last = id; ++observed.received;
            observed.latency.push_back(std::chrono::duration<double, std::milli>(arrived - sent[id]).count());
            changed.notify_all();
          }
        } catch (const pvxs::client::Disconnect&) {
          std::lock_guard<std::mutex> guard(mutex);
          if (observations[index].initial) failure = "PVA disconnected during measurement";
          changed.notify_all();
        } catch (const std::exception& error) {
          std::lock_guard<std::mutex> guard(mutex); failure = error.what(); changed.notify_all();
        }
      }).exec());
    }
    for (auto& client : clients) client.hurryUp();
    RA_ArgsAdd arguments; arguments.trim = 16; arguments.approximateTrim = false;
    std::vector<uint32_t> payload(elements);
    // An explicit zero sample readies both scalar and array monitors. It is
    // excluded from sample/latency accounting and has the same validated shape.
    for (uint32_t i = 0; i < elements; ++i) payload[i] = i;
    require((elements == 1 ? producer.addSingleValue<uint32_t>("data", 0, arguments)
                           : producer.addSingleList("data", payload, arguments)).ok(), "initial sample rejected");
    {
      std::unique_lock<std::mutex> lock(mutex);
      const auto ready = changed.wait_for(lock, 5s, [&] {
        return !failure.empty() || std::all_of(observations.begin(), observations.end(), [](const auto& o) { return o.initial; });
      });
      if (!ready) throw std::runtime_error("initial monitors timed out (" + std::to_string(std::count_if(
          observations.begin(), observations.end(), [](const auto& o) { return o.initial; })) +
          " of " + std::to_string(count) + " ready)");
      if (!failure.empty()) throw std::runtime_error(failure);
    }
    const auto start = Clock::now();
    const auto period = std::chrono::nanoseconds(1000000000 / rate);
    const auto deadline = start + period * samples + 10s;
    double maxLate = 0;
    for (uint32_t id = 1; id <= samples; ++id) {
      const auto due = start + period * (id - 1);
      std::this_thread::sleep_until(due);
      require(Clock::now() < deadline, "producer exceeded total deadline");
      maxLate = std::max(maxLate, std::chrono::duration<double, std::milli>(Clock::now() - due).count());
      for (uint32_t i = 0; i < elements; ++i) payload[i] = id + i;
      { std::lock_guard<std::mutex> guard(mutex); if (!failure.empty()) throw std::runtime_error(failure); sent[id] = Clock::now(); }
      require((elements == 1 ? producer.addSingleValue<uint32_t>("data", id, arguments)
                             : producer.addSingleList("data", payload, arguments)).ok(), "Redis write failed; not retried");
    }
    const auto producedSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(changed.wait_for(lock, 5s, [&] {
        return !failure.empty() || std::all_of(observations.begin(), observations.end(), [&](const auto& o) { return o.last == samples; });
      }), "final values did not converge");
      if (!failure.empty()) throw std::runtime_error(failure);
    }
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    uint64_t received = 0, duplicates = 0, serverSquash = 0, clientSquash = 0, maxQueue = 0;
    std::vector<double> latency;
    for (uint32_t i = 0; i < count; ++i) {
      monitors[i]->cancel(); monitors[i]->stats(observations[i].queue);
      const auto& o = observations[i];
      received += o.received; duplicates += o.duplicates;
      serverSquash += o.queue.nSrvSquash; clientSquash += o.queue.nCliSquash;
      maxQueue = std::max(maxQueue, uint64_t(o.queue.maxQueue));
      latency.insert(latency.end(), o.latency.begin(), o.latency.end());
    }
    for (auto& client : clients) client.close();
    std::sort(latency.begin(), latency.end());
    require(!latency.empty(), "no observed samples");
    const auto quantile = [&](double q) { return latency[size_t((latency.size() - 1) * q)]; };
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    const auto rss = usage.ru_maxrss;
#else
    const auto rss = usage.ru_maxrss * 1024;
#endif
    std::ofstream report(output);
    report << "{\"samples\":" << samples << ",\"clients\":" << count << ",\"elements\":" << elements
           << ",\"payload_bytes\":" << size_t(elements) * 4 << ",\"rate_target_hz\":" << rate
           << ",\"produced_hz\":" << samples / producedSeconds << ",\"elapsed_seconds\":" << seconds
           << ",\"received\":" << received << ",\"missed_updates\":" << uint64_t(samples) * count - received
           << ",\"duplicate_updates\":" << duplicates << ",\"final_values_converged\":true"
           << ",\"latency_p50_ms\":" << quantile(.5) << ",\"latency_p95_ms\":" << quantile(.95)
           << ",\"latency_p99_ms\":" << quantile(.99) << ",\"producer_max_late_ms\":" << maxLate
           << ",\"monitor_queue_limit\":16,\"monitor_queue_peak\":" << maxQueue
           << ",\"server_squash_events\":" << serverSquash << ",\"client_squash_updates\":" << clientSquash
           << ",\"redis_history_entries\":16,\"client_peak_rss_bytes\":" << rss << "}\n";
    require(report.good(), "unable to write report");
    std::cout << received << " updates received; " << uint64_t(samples) * count - received
              << " intermediate updates missed/coalesced; final values converged\n";
  } catch (const std::exception& error) {
    std::cerr << "capacity measurement failed: " << error.what() << '\n';
    return 1;
  }
}
