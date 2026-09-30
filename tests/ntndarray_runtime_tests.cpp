#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/ntndarray.h"
#include "RedisAdapter.hpp"
#include <alarm.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <thread>
#include <sys/socket.h>
#include <pvxs/client.h>
#include <pvxs/server.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(2ms);
  }
}
RedisAdapter::Attrs frame(int32_t id, uint8_t pixel, size_t size = 4) {
  return {{"schema", kNTNDArrayRedisSchema}, {"data_type", "1"}, {"shape", "[" + std::to_string(size) + "]"},
          {"color_mode", "mono"}, {"unique_id", std::to_string(id)}, {"_", std::string(size, char(pixel))}};
}

int main() {
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  auto redis = std::make_shared<RedisAdapter>("ndarray", options);
  RedisAdapter producer("ndarray", options);
  RedisBackendRegistry backends{{"default", redis}};
  ServerConfig config; config.instance = "ndarray"; config.nameSpace = "TEST";
  PVConfig image; image.name = "one"; image.kind = PVKind::NTNDArray;
  image.shape = Shape::Array; image.read = {"default", "frame"};
  auto stats = std::make_shared<RuntimeStats>();
  const auto make = [&](const PVConfig& pv, OperationLimitsConfig limits = OperationLimitsConfig{}) {
    return makeRuntime(config, pv, backends, {}, 1, {}, limits, stats);
  };
  const auto send = [&](const RedisAdapter::Attrs& fields) {
    RA_ArgsAdd args; args.trim = 32; args.approximateTrim = false;
    const auto time = producer.addSingleValue("frame", fields, args);
    assert(time.ok()); return time;
  };
  const auto ready = [](const std::shared_ptr<PVRuntimeBase>& pv, int32_t id, uint8_t pixel) {
    const auto value = pv->sharedPV().fetch();
    const auto data = value["value"].as<pvxs::shared_array<const uint8_t>>();
    return value["uniqueId"].as<int32_t>() == id && !data.empty() && data[0] == pixel &&
           value["alarm.severity"].as<int32_t>() == epicsSevNone;
  };
  const auto firstTime = send(frame(1, 1));
  auto one = make(image); one->activate();
  image.name = "two";
  auto two = make(image); two->activate();
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  server.addPV("TEST:one", one->sharedPV()); server.addPV("TEST:two", two->sharedPV()); server.start();
  auto client = server.clientConfig().build();
  const auto held = client.get("TEST:one").exec()->wait(2.);
  assert(held.idStartsWith("epics:nt/NTNDArray:1.0"));
  assert(held["uniqueId"].as<int32_t>() == 1);
  assert(held["dataTimeStamp.secondsPastEpoch"].as<int64_t>() == firstTime.value / 1000000000);
  assert(held["dataTimeStamp.nanoseconds"].as<int32_t>() == firstTime.value % 1000000000);
  send(frame(2, 2));
  eventually([&] { return ready(one, 2, 2) && ready(two, 2, 2); });
  assert(held["value"].as<pvxs::shared_array<const uint8_t>>()[0] == 1); // previously delivered pixels remain owned

  // Discard a staged same-key owner; it must not cancel either live reader.
  image.name = "discarded";
  { auto staged = make(image); }
  send(frame(3, 3));
  eventually([&] { return ready(one, 3, 3) && ready(two, 3, 3); });
  image.name = "two"; image.aliases = {"TEST:alias"};
  { auto discardedUpdate = two->prepareReconfigure(image, 2); }
  send(frame(4, 4));
  eventually([&] { return ready(one, 4, 4) && ready(two, 4, 4); });

  // Malformed input retains every last-good frame field except the alarm/post timestamp.
  const auto good = client.get("TEST:two").exec()->wait(2.);
  auto bad = frame(5, 5); bad["_"] = "short";
  send(bad);
  eventually([&] { return two->sharedPV().fetch()["alarm.severity"].as<int32_t>() == epicsSevInvalid; });
  const auto invalid = client.get("TEST:two").exec()->wait(2.);
  assert(invalid["uniqueId"].as<int32_t>() == 4);
  assert(invalid["dimension[0].size"].as<int32_t>() == 4);
  assert(invalid["value"].as<pvxs::shared_array<const uint8_t>>()[0] == 4);
  assert(invalid["dataTimeStamp.secondsPastEpoch"].as<int64_t>() == good["dataTimeStamp.secondsPastEpoch"].as<int64_t>());
  assert(invalid["dataTimeStamp.nanoseconds"].as<int32_t>() == good["dataTimeStamp.nanoseconds"].as<int32_t>());
  eventually([&] { return stats->ndarrayInvalidFrames == 2; });
  image.name = "invalid-staging";
  { auto staged = make(image); assert(staged->sharedPV().fetch()["alarm.severity"].as<int32_t>() == epicsSevInvalid); }
  assert(stats->ndarrayInvalidFrames == 2); // rejected staging has no global counter effect

  // Replacement overlap, followed by old-owner retirement, leaves the second PV alive.
  send(frame(6, 6));
  eventually([&] { return ready(one, 6, 6) && ready(two, 6, 6); });
  image.name = "replacement";
  auto replacement = make(image); replacement->activate();
  one->deactivate("replaced"); server.removePV("TEST:one"); one.reset();
  server.addPV("TEST:replacement", replacement->sharedPV());
  send(frame(7, 7));
  eventually([&] { return ready(replacement, 7, 7) && ready(two, 7, 7); });
  assert(client.get("TEST:two").exec()->wait(2.)["uniqueId"].as<int32_t>() == 7);
  try { client.put("TEST:two").set("uniqueId", 8).exec()->wait(2.); assert(false); }
  catch (const pvxs::client::RemoteError&) {}

  // The global payload budget also constrains imaging, independently of its local cap.
  image.name = "limited";
  OperationLimitsConfig limits; limits.maxPayloadBytes = 4;
  auto limited = make(image, limits); limited->activate();
  send(frame(8, 8, 8));
  eventually([&] { return ready(two, 8, 8) && limited->sharedPV().fetch()["alarm.severity"].as<int32_t>() == epicsSevInvalid; });
  assert(limited->sharedPV().fetch()["uniqueId"].as<int32_t>() == 7);
  limited->deactivate("done");
  const auto maximum = std::numeric_limits<int32_t>::max(), minimum = std::numeric_limits<int32_t>::min();
  send(frame(maximum - 1, 9));
  eventually([&] { return ready(two, maximum - 1, 9) && ready(replacement, maximum - 1, 9); });
  const auto baseline = stats->ndarraySkippedFrames.load();
  const auto discontinuities = stats->ndarrayDiscontinuities.load();
  for (const auto id : {maximum, minimum, minimum + 1}) {
    send(frame(id, 10));
    eventually([&] { return ready(two, id, 10) && ready(replacement, id, 10); });
  }
  assert(stats->ndarraySkippedFrames == baseline);
  assert(stats->ndarrayDiscontinuities == discontinuities);
  send(frame(maximum, 11));
  eventually([&] { return ready(two, maximum, 11) && ready(replacement, maximum, 11); });
  assert(stats->ndarraySkippedFrames == baseline);
  assert(stats->ndarrayDiscontinuities == discontinuities + 2);
  send(frame(maximum - 1, 12));
  eventually([&] { return ready(two, maximum - 1, 12) && ready(replacement, maximum - 1, 12); });
  send(frame(minimum + 2, 13));
  eventually([&] { return ready(two, minimum + 2, 13) && ready(replacement, minimum + 2, 13); });
  assert(stats->ndarraySkippedFrames == baseline + 6);  // three missed IDs per canonical runtime
  send(frame(-2000000000, 14));
  eventually([&] { return ready(two, -2000000000, 14) && ready(replacement, -2000000000, 14); });
  const auto beforeRestart = stats->ndarrayDiscontinuities.load();
  send(frame(1, 15));
  eventually([&] { return ready(two, 1, 15) && ready(replacement, 1, 15); });
  assert(stats->ndarraySkippedFrames == baseline + 6);
  assert(stats->ndarrayDiscontinuities == beforeRestart + 2);

  // A policy reload retains the runtime and applies the new bound only to it.
  auto stricter = two->config(); stricter.maxFrameGap = 2;
  two->reconfigure(stricter, 2);
  assert(ready(two, 1, 15));
  const auto policySkipped = stats->ndarraySkippedFrames.load();
  const auto policyDiscontinuities = stats->ndarrayDiscontinuities.load();
  send(frame(5, 16));
  eventually([&] { return ready(two, 5, 16) && ready(replacement, 5, 16); });
  assert(stats->ndarraySkippedFrames == policySkipped + 3);  // only the unchanged policy counts the gap
  assert(stats->ndarrayDiscontinuities == policyDiscontinuities + 1);
  image.name = "discarded-discontinuity";
  const auto beforeStaging = stats->ndarrayDiscontinuities.load();
  {
    auto staged = make(image);
    send(frame(-2000000000, 17));
    eventually([&] { return ready(two, -2000000000, 17) && ready(replacement, -2000000000, 17)
                           && ready(staged, -2000000000, 17); });
    assert(stats->ndarrayDiscontinuities == beforeStaging + 2);
  }
  assert(stats->ndarrayDiscontinuities == beforeStaging + 2);  // discarded staging stays uncounted
  client.close(); server.stop(); two->deactivate("done"); replacement->deactivate("done");
}
