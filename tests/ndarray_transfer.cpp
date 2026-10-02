// Deterministic Redis -> external IOC -> PVA monitor acceptance. No server is
// linked into the test path and no existing stream is cleared or reused.
#include "RedisAdapter.hpp"
#include "redis_pvxs_ioc/ntndarray.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <pvxs/client.h>

using namespace redis_pvxs_ioc;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

uint32_t number(const std::string& value, uint32_t maximum) {
  uint32_t result = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !result || result > maximum)
    throw std::runtime_error("invalid numeric option");
  return result;
}
int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
void require(bool condition, const char* error) { if (!condition) throw std::runtime_error(error); }

int main(int argc, char** argv) {
  try {
    std::string host = "127.0.0.1", base, pv, address, output;
    uint32_t port = 0, frames = 600, width = 1920, height = 1080, fps = 10;
    for (int i = 1; i < argc; ++i) {
      const std::string option = argv[i];
      if (i + 1 == argc) throw std::runtime_error("missing option value");
      const std::string value = argv[++i];
      if (option == "--redis-host") host = value;
      else if (option == "--redis-port") port = number(value, 65535);
      else if (option == "--base-key") base = value;
      else if (option == "--pv") pv = value;
      else if (option == "--pva-address") address = value;
      else if (option == "--output") output = value;
      else if (option == "--frames") frames = number(value, 100000);
      else if (option == "--width") width = number(value, 8192);
      else if (option == "--height") height = number(value, 8192);
      else if (option == "--fps") fps = number(value, 1000);
      else throw std::runtime_error("unknown option: " + option);
    }
    const auto pixels = size_t(width) * height;
    require(port && !base.empty() && !pv.empty() && !address.empty() && !output.empty(),
            "required: --redis-port --base-key --pv --pva-address --output");
    require(pixels <= 32u * 1024u * 1024u, "frame exceeds 32 MiB test bound");
    require(!std::filesystem::exists(output), "report already exists; use a new evidence path");
    RA_Options options; options.cxn.host = host; options.cxn.port = port;
    RedisAdapter producer(base, options);
    require(producer.connected(), "test Redis unavailable");
    require(!producer.getStreamSnapshot("image").present(), "test stream is not empty; choose an isolated base key");
    pvxs::client::Config config; config.autoAddrList = false; config.addressList = {address};
    auto client = config.build();
    std::mutex mutex;
    std::condition_variable changed;
    bool initial = false;
    uint32_t received = 0;
    std::string failure;
    std::vector<int64_t> timestamps(frames + 1);
    std::vector<double> latency;
    latency.reserve(frames);
    auto monitor = client.monitor(pv).record("queueSize", uint32_t(16)).record("pipeline", true)
        .maskConnected(true).maskDisconnected(false).event([&](pvxs::client::Subscription& subscription) {
      try {
        while (const auto value = subscription.pop()) {
          const auto arrival = nowNs();
          require(value.idStartsWith("epics:nt/NTNDArray:1.0"), "wrong normative type");
          const auto id = value["uniqueId"].as<int32_t>();
          std::lock_guard<std::mutex> guard(mutex);
          if (id == 0 && !received) { initial = true; changed.notify_all(); continue; }
          require(id == int32_t(received + 1) && uint32_t(id) <= frames, "frame gap, duplicate or reordering");
          require(value["alarm.severity"].as<int32_t>() == 0, "invalid frame alarm");
          require(value["dimension[0].size"].as<int32_t>() == int32_t(width) &&
                  value["dimension[1].size"].as<int32_t>() == int32_t(height), "wrong frame dimensions");
          const auto data = value["value"].as<pvxs::shared_array<const uint8_t>>();
          require(data.size() == pixels, "wrong payload length");
          for (size_t pixel = 0; pixel < pixels; ++pixel)
            require(data[pixel] == uint8_t(pixel + id), "pixel corruption");
          const auto timestamp = value["dataTimeStamp.secondsPastEpoch"].as<int64_t>() * 1000000000 +
                                 value["dataTimeStamp.nanoseconds"].as<int32_t>();
          require(timestamp == timestamps[id], "acquisition timestamp changed");
          latency.push_back(double(arrival - timestamp) / 1e6);
          ++received; changed.notify_all();
        }
      } catch (const pvxs::client::Disconnect&) {
        std::lock_guard<std::mutex> guard(mutex);
        if (initial) failure = "PVA disconnected during transfer";
        changed.notify_all();
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> guard(mutex); failure = error.what(); changed.notify_all();
      }
    }).exec();
    struct CancelMonitor {
      std::shared_ptr<pvxs::client::Subscription>& subscription;
      ~CancelMonitor() { subscription->cancel(); } // joins any callback before its captures leave scope
    } cancel{monitor};
    client.hurryUp();
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(changed.wait_for(lock, 5s, [&] { return initial || !failure.empty(); }), "initial image monitor timed out");
      if (!failure.empty()) throw std::runtime_error(failure);
    }
    RedisAdapter::Attrs fields{{"schema", kNTNDArrayRedisSchema}, {"data_type", "1"},
                              {"shape", "[" + std::to_string(width) + "," + std::to_string(height) + "]"},
                              {"color_mode", "mono"}, {"time_source", "host"}, {"_", std::string(pixels, '\0')}};
    RA_ArgsAdd arguments; arguments.trim = 16; arguments.approximateTrim = false;
    const auto start = Clock::now();
    const auto period = std::chrono::nanoseconds(1000000000 / fps);
    double producerMaxLateMs = 0;
    for (uint32_t id = 1; id <= frames; ++id) {
      std::this_thread::sleep_until(start + period * (id - 1));
      producerMaxLateMs = std::max(producerMaxLateMs,
          std::chrono::duration<double, std::milli>(Clock::now() - (start + period * (id - 1))).count());
      auto& payload = fields.at("_");
      for (size_t pixel = 0; pixel < pixels; ++pixel) payload[pixel] = char(uint8_t(pixel + id));
      fields["unique_id"] = std::to_string(id);
      const auto timestamp = nowNs();
      { std::lock_guard<std::mutex> guard(mutex); if (!failure.empty()) throw std::runtime_error(failure); timestamps[id] = timestamp; }
      arguments.time = RA_Time(timestamp);
      require(producer.addSingleValue("image", fields, arguments).ok(), "Redis did not accept frame; not retried");
    }
    {
      std::unique_lock<std::mutex> lock(mutex);
      require(changed.wait_for(lock, 5s, [&] { return received == frames || !failure.empty(); }), "missing final frames");
      if (!failure.empty()) throw std::runtime_error(failure);
    }
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    monitor->cancel(); client.close();
    std::sort(latency.begin(), latency.end());
    const auto quantile = [&](double fraction) { return latency[size_t((latency.size() - 1) * fraction)]; };
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    const auto rssBytes = usage.ru_maxrss;
#else
    const auto rssBytes = usage.ru_maxrss * 1024;
#endif
    std::ofstream report(output);
    report << "{\"passed\":true,\"frames\":" << received << ",\"width\":" << width << ",\"height\":" << height
           << ",\"fps_target\":" << fps << ",\"format\":\"Mono8\",\"pixels_checked\":" << pixels * frames
           << ",\"unexplained_gaps\":0,\"duplicates\":0,\"elapsed_seconds\":" << seconds
           << ",\"latency_p50_ms\":" << quantile(.5) << ",\"latency_p95_ms\":" << quantile(.95)
           << ",\"latency_p99_ms\":" << quantile(.99) << ",\"producer_max_late_ms\":" << producerMaxLateMs
           << ",\"monitor_queue_entries\":16,\"redis_history_entries\":16,\"client_peak_rss_bytes\":" << rssBytes << "}\n";
    require(report.good(), "unable to write report");
    std::cout << received << " ordered, exact " << width << 'x' << height << " Mono8 frames passed\n";
  } catch (const std::exception& error) {
    std::cerr << "ndarray transfer failed: " << error.what() << '\n';
    return 1;
  }
}
