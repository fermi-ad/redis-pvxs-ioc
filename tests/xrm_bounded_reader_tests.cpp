#include "redis_pvxs_ioc/xrm_latest_reader.h"

#include <sw/redis++/redis++.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class Predicate> void eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  assert(predicate());
}

int main() {
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  RedisConfig config;
  config.baseKey = "xrm-bounded-test";
  config.port = static_cast<uint16_t>(std::stoi(port));
  sw::redis::ConnectionOptions options;
  options.host = config.host;
  options.port = config.port;
  sw::redis::Redis redis(options);

  auto publish = [&](const std::string& key, uint64_t batch, size_t payloadBytes = 8, int maxlen = 1000) {
    const auto count = std::to_string(batch);
    return redis.command("XADD", "{" + config.baseKey + "}:" + key, "MAXLEN", "=",
        std::to_string(maxlen), "*", "_", std::string(payloadBytes, 'x'),
        "schema", "fermi-ad/xrm-waveform:1", "session", "session1", "connection", "1",
        "batch", count, "channel", "1", "sample_count", "4", "sample_bytes", "2",
        "sample_rate_hz", "1000", "source_validity", "unknown", "acq_time_valid", "0");
  };

  // Startup and normal consumption: no callback before start, and all
  // retained entries are delivered in order while the consumer keeps up.
  publish("ordered", 1);
  std::mutex orderedMutex;
  std::vector<uint64_t> ordered;
  XrmLatestReader normal(config, "ordered", 1024, 5,
      [&](const auto&, const auto&, const auto& entries, const auto&) {
        assert(entries.size() == 1);
        std::lock_guard<std::mutex> lock(orderedMutex);
        ordered.push_back(std::stoull(entries[0].second.at("batch")));
      });
  const auto first = normal.snapshot();
  assert(first.present() && first.fields.at("batch") == "1");
  publish("ordered", 2);
  { std::lock_guard<std::mutex> lock(orderedMutex); assert(ordered.empty()); }
  normal.start();
  for (int i = 3; i <= 50; ++i) publish("ordered", i);
  eventually([&] { std::lock_guard<std::mutex> lock(orderedMutex); return ordered.size() == 49; });
  {
    std::lock_guard<std::mutex> lock(orderedMutex);
    for (size_t i = 0; i < ordered.size(); ++i) assert(ordered[i] == i + 2);
  }
  const auto orderedTail = normal.tailMetadata();
  assert(orderedTail.present && orderedTail.session == "session1" && orderedTail.batch == "50" &&
         orderedTail.sampleCount == "4" && orderedTail.sampleRate == "1000");
  normal.reset();

  // Hold the consumer while a fast producer publishes 5,000 entries into a
  // stream retaining only eight. While blocked, the reader cannot accumulate
  // callbacks or payloads. After release, the missing sequence is observable.
  std::mutex mutex;
  std::condition_variable changed;
  bool callbackEntered = false, release = false;
  std::vector<uint64_t> received;
  XrmLatestReader slow(config, "slow", 1024, 5,
      [&](const auto&, const auto&, const auto& entries, const auto&) {
        std::unique_lock<std::mutex> lock(mutex);
        received.push_back(std::stoull(entries[0].second.at("batch")));
        if (received.size() == 1) {
          callbackEntered = true;
          changed.notify_all();
          changed.wait(lock, [&] { return release; });
        }
      });
  assert(!slow.snapshot().present());
  slow.start();
  publish("slow", 1, 512, 8);
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return callbackEntered; }));
  }
  for (int i = 2; i <= 5001; ++i) publish("slow", i, 512, 8);
  const auto paused = slow.status();
  assert(paused.entries == 1 && paused.callbacks == 0);
  {
    std::lock_guard<std::mutex> lock(mutex);
    assert(received.size() == 1 && received[0] == 1);
    release = true;
  }
  changed.notify_all();
  eventually([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return !received.empty() && received.back() == 5001;
  });
  {
    std::lock_guard<std::mutex> lock(mutex);
    assert(received.size() <= 9 && received[1] > received[0] + 1);
    assert(received[1] >= 4994);
  }
  assert(slow.tailMetadata().batch == "5001");
  slow.reset();

  // Oversized payload, oversized metadata, excessive fields, and duplicate
  // fields are rejected server-side and advance the cursor exactly once.
  publish("limits", 1, 2 * 1024 * 1024);
  std::mutex limitMutex;
  uint64_t acceptedBatch = 0, acceptedRejections = 0, rejectedCallbacks = 0;
  XrmLatestReader limited(config, "limits", 1024, 5,
      [&](const auto&, const auto&, const auto& entries, const auto& metadata) {
        std::lock_guard<std::mutex> lock(limitMutex);
        if (entries[0].second.empty()) { ++rejectedCallbacks; return; }
        acceptedBatch = std::stoull(entries[0].second.at("batch"));
        acceptedRejections = metadata.readRejections;
      });
  const auto oversized = limited.snapshot();
  assert(oversized.connected && oversized.rejected && oversized.fields.empty() && oversized.id != "0-0");
  assert(limited.status().readRejections == 1);
  limited.start();
  redis.command("XADD", "{" + config.baseKey + "}:limits", "*", "_", "x", "extra", std::string(5000, 'm'));
  eventually([&] { return limited.status().readRejections == 2; });
  std::vector<std::string> excessive{"XADD", "{" + config.baseKey + "}:limits", "*"};
  for (int i = 0; i < 33; ++i) { excessive.push_back("field" + std::to_string(i)); excessive.push_back("x"); }
  redis.command(excessive.begin(), excessive.end());
  eventually([&] { return limited.status().readRejections == 3; });
  redis.command("XADD", "{" + config.baseKey + "}:limits", "*", "_", "x", "batch", "1", "batch", "2");
  eventually([&] { return limited.status().readRejections == 4; });
  std::this_thread::sleep_for(50ms);
  assert(limited.status().readRejections == 4);
  publish("limits", 5);
  eventually([&] { std::lock_guard<std::mutex> lock(limitMutex); return acceptedBatch == 5; });
  { std::lock_guard<std::mutex> lock(limitMutex); assert(acceptedRejections == 4 && rejectedCallbacks == 3); }
  assert(limited.tailMetadata().present && limited.tailMetadata().batch == "5");

  // Deleting and recreating a stream changes the reader epoch and permits the
  // replacement stream to recover without reusing the previous cursor.
  redis.command("DEL", "{" + config.baseKey + "}:limits");
  eventually([&] { return limited.status().streamKind == RedisAdapter::StreamKind::Missing; });
  assert(limited.status().epoch == 2 && limited.status().disappearances == 1);
  publish("limits", 6);
  eventually([&] { std::lock_guard<std::mutex> lock(limitMutex); return acceptedBatch == 6; });
  assert(limited.status().epoch == 2);
  limited.reset();

  // A replacement with a lower stream ID can occur entirely between polls;
  // the reader detects it and consumes the new stream in a new epoch.
  redis.command("XADD", "{" + config.baseKey + "}:backward", "1000-0", "_", "old");
  std::mutex resetMutex;
  uint64_t resetEpoch = 0;
  XrmLatestReader backward(config, "backward", 1024, 50,
      [&](const auto&, const auto&, const auto&, const auto& metadata) {
        std::lock_guard<std::mutex> lock(resetMutex);
        resetEpoch = metadata.epoch;
      });
  assert(backward.snapshot().id == "1000-0");
  redis.command("DEL", "{" + config.baseKey + "}:backward");
  redis.command("XADD", "{" + config.baseKey + "}:backward", "1-0", "_", "new");
  backward.start();
  eventually([&] { std::lock_guard<std::mutex> lock(resetMutex); return resetEpoch == 2; });
  assert(backward.status().streamResets == 1);
  backward.reset();

  // Script permissions are explicit: an EVAL denial is a read rejection,
  // never an excuse to fall back to an uncapped client-side fetch.
  redis.command("ACL", "SETUSER", "bounded-test-user", "on", ">bounded-test-password",
                "resetkeys", "~{xrm-bounded-test}:*", "-@all", "+type", "+xrange", "+xrevrange");
  auto restrictedConfig = config;
  restrictedConfig.user = "bounded-test-user";
  restrictedConfig.password = "bounded-test-password";
  publish("acl", 1);
  std::mutex aclMutex;
  bool aclRecovered = false;
  XrmLatestReader acl(restrictedConfig, "acl", 1024, 10,
      [&](const auto&, const auto&, const auto& entries, const auto& metadata) {
        std::lock_guard<std::mutex> lock(aclMutex);
        aclRecovered = entries[0].second.at("batch") == "1" && metadata.readRejections > 0;
      });
  const auto denied = acl.snapshot();
  assert(denied.rejected && denied.fields.empty() && acl.status().readRejections == 1);
  acl.start();
  redis.command("ACL", "SETUSER", "bounded-test-user", "+eval");
  eventually([&] { std::lock_guard<std::mutex> lock(aclMutex); return aclRecovered; });
  acl.reset();
  redis.command("ACL", "DELUSER", "bounded-test-user");

  std::cout << "Bounded XRM reader ordering, overload loss, size limits, ACL and resets passed\n";
}
