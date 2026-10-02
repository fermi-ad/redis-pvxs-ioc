#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/ntndarray.h"
#include "redis_pvxs_ioc/config_diff.h"
#include <alarm.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class F> void eventuallyImpl(F predicate, const char* expression) {
  const auto deadline = SourceClock::now() + 6s;
  while (!predicate()) {
    if (SourceClock::now() >= deadline) {
      std::cerr << "timed out: " << expression << '\n'; assert(false);
    }
    std::this_thread::sleep_for(5ms);
  }
}
#define eventually(...) eventuallyImpl(__VA_ARGS__, #__VA_ARGS__)

RedisAdapter::Attrs scalar(double value) {
  std::string bytes(sizeof(value), '\0');
  std::memcpy(bytes.data(), &value, sizeof(value));
  return {{"_", bytes}};
}

void delayedReadEvidence(sw::redis::Redis& control, uint16_t port, PVKind kind) {
  const auto base = kind == PVKind::NTNDArray ? "health-delayed-frame" : "health-delayed-scalar";
  const std::string user = std::string(base) + "-reader", key = "{" + std::string(base) + "}:read";
  control.command<void>("ACL", "SETUSER", user, "reset", "on", "nopass", "~{" + std::string(base) + "}:*", "+@all");
  const auto fields = [kind](int value) {
    if (kind == PVKind::Value) return scalar(value);
    return RedisAdapter::Attrs{{"schema", kNTNDArrayRedisSchema}, {"data_type", "1"}, {"shape", "[4]"},
        {"color_mode", "mono"}, {"unique_id", std::to_string(value)}, {"_", std::string(4, char(value))}};
  };
  const auto send = [&](int value) {
    const auto data = fields(value);
    const auto id = std::to_string(value) + "-0";
    assert(control.xadd(key, id, data.begin(), data.end()) == id);
  };
  send(1);
  RA_Options options; options.cxn.port = port; options.cxn.user = user;
  options.cxn.timeout = 100; options.workers = 1;
  auto adapter = std::make_shared<RedisAdapter>(base, options);
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  RedisAdapter::SubscriptionOptions selection; selection.afterId = "1-0"; selection.probeMs = 0;
  // The sibling callback precedes the IOC registration on the same worker/key.
  // Hold this actual valid read so the IOC batch remains queued while the
  // independent read thread observes an actual ACL XREAD rejection.
  auto blocker = adapter->subscribeStreamWithMetadata("read", [&](const auto&, const auto&, const auto& batch, const auto&) {
    if (batch.front().first != "2-0") return;
    std::unique_lock<std::mutex> lock(mutex);
    entered = true; changed.notify_all();
    assert(changed.wait_for(lock, 6s, [&] { return released; }));
  }, selection);
  ServerConfig server; server.instance = base; server.nameSpace = "TEST";
  PVConfig config; config.name = "read"; config.kind = kind; config.read = {"default", "read"};
  RedisBackendConfigs policies{{"default", RedisConfig{}}}; policies.at("default").readerProbeMs = 0;
  auto runtime = makeRuntime(server, config, {{"default", adapter}}, {}, 1, {}, {}, {}, policies);
  runtime->activate();
  eventually([&] { return runtime->sourceHealth()[0].ready; });
  send(2);
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 3s, [&] { return entered; }));
  }
  eventually([&] { return runtime->sourceHealth()[0].reader.observedCursor == "2-0"; });
  control.command<void>("ACL", "SETUSER", user, "-xread");
  eventually([&] { return runtime->sourceHealth()[0].reader.readRejections > 0; });
  const auto rejected = runtime->sourceHealth()[0];
  assert(!rejected.ready && rejected.reader.epoch == 1);
  {
    std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all();
  }
  eventually([&] { return runtime->sourceHealth()[0].lastValidCursor == "2-0"; });
  const auto delayed = runtime->sourceHealth()[0];
  assert(delayed.valid && !delayed.ready && delayed.state == "read-rejected");
  assert(delayed.reader.readRejections >= rejected.reader.readRejections && delayed.reader.epoch == 1);
  const auto heldValue = runtime->sharedPV().fetch();
  assert(heldValue["alarm.severity"].as<int32_t>() == epicsSevInvalid);
  if (kind == PVKind::Value) assert(heldValue["value"].as<double>() == 2);
  else assert(heldValue["uniqueId"].as<int32_t>() == 2);
  // Restoring permission alone does not fabricate a new successful sample.
  control.command<void>("ACL", "SETUSER", user, "+xread");
  assert(!runtime->sourceHealth()[0].ready);
  send(3);
  eventually([&] { const auto s = runtime->sourceHealth()[0]; return s.ready && s.lastValidCursor == "3-0"; });
  assert(runtime->sourceHealth()[0].reader.epoch == 1);
  runtime->deactivate("complete"); blocker.reset(); runtime.reset(); adapter.reset();
  control.command<long long>("ACL", "DELUSER", user);
}

int main() {
  RA_Options options;
  options.cxn.port = std::stoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  options.cxn.timeout = 100;
  sw::redis::ConnectionOptions connection;
  connection.host = "127.0.0.1"; connection.port = options.cxn.port;
  sw::redis::Redis control(connection);
  delayedReadEvidence(control, options.cxn.port, PVKind::Value);
  delayedReadEvidence(control, options.cxn.port, PVKind::NTNDArray);
  control.command<void>("ACL", "SETUSER", "runtime-health", "reset", "on", "nopass", "~{source-health}:*", "+@all");
  options.cxn.user = "runtime-health";
  const std::string base = "source-health";
  const auto key = [&](const std::string& sub) { return "{" + base + "}:" + sub; };
  const auto send = [&](const std::string& sub, const std::string& id, RedisAdapter::Attrs fields) {
    assert(control.xadd(key(sub), id, fields.begin(), fields.end()) == id);
  };
  auto adapter = std::make_shared<RedisAdapter>(base, options);
  RedisBackendRegistry backends{{"default", adapter}};
  RedisBackendConfigs policies{{"default", RedisConfig{}}};
  policies.at("default").readerProbeMs = 100;
  ServerConfig server; server.instance = "health"; server.nameSpace = "TEST";
  const auto make = [&](const PVConfig& config) {
    return makeRuntime(server, config, backends, {}, 1, {}, {}, {}, policies);
  };

  // Exact ordering IDs are independent of the legacy source time encoding.
  const std::string first = "1700000000123-1234567";
  send("read", first, scalar(42));
  PVConfig config; config.name = "value"; config.read = {"default", "read"};
  auto runtime = make(config);
  assert(!runtime->sourceHealth()[0].ready); // A staged owner is not active readiness.
  runtime->activate();
  eventually([&] { return runtime->sourceHealth()[0].ready; });
  auto health = runtime->sourceHealth()[0];
  assert(health.reader.cursor == first && health.lastValidCursor == first && health.lastValidEpoch == 1);
  const auto initial = runtime->sharedPV().fetch();
  const auto timestamp = RA_Time(first).value;
  assert(initial["timeStamp.secondsPastEpoch"].as<int64_t>() == timestamp / 1000000000);
  assert(initial["timeStamp.nanoseconds"].as<int32_t>() == timestamp % 1000000000);
  // No configured cadence means an old, valid idle source is not arbitrarily stale.
  assert(runtime->sourceHealth(SourceClock::now() + 24h)[0].ready);

  send("read", "1700000000124-9", {{"_", "bad"}});
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 1; });
  health = runtime->sourceHealth()[0];
  assert(!health.valid && !health.ready && health.state == "invalid");
  auto retained = runtime->sharedPV().fetch();
  assert(retained["value"].as<double>() == 42);
  assert(retained["timeStamp.secondsPastEpoch"].as<int64_t>() == initial["timeStamp.secondsPastEpoch"].as<int64_t>());
  assert(retained["timeStamp.nanoseconds"].as<int32_t>() == initial["timeStamp.nanoseconds"].as<int32_t>());
  assert(health.reader.cursor == "1700000000124-9" && health.lastValidCursor == first);
  send("read", "1700000000125-10", scalar(43));
  eventually([&] { return runtime->sourceHealth()[0].ready; });

  // Health policy is a retained-runtime update, and counts survive that update.
  const auto cursor = runtime->sourceHealth()[0].reader.cursor;
  config.sourceHealth.staleAfterMs = 100;
  runtime->reconfigure(config, 2);
  health = runtime->sourceHealth(SourceClock::now() + 200ms)[0];
  assert(health.stale && !health.ready && health.staleTransitions == 1 && health.invalidSamples == 1);
  assert(runtime->sourceHealth(SourceClock::now() + 300ms)[0].staleTransitions == 1);
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 43);
  assert(runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  config.sourceHealth.staleAfterMs = 0;
  runtime->reconfigure(config, 3);
  assert(runtime->sourceHealth()[0].ready && runtime->sourceHealth()[0].reader.cursor == cursor);

  // Deletion invalidates readiness without discarding last-good values/time.
  const auto good = runtime->sharedPV().fetch();
  control.del(key("read"));
  eventually([&] { return runtime->sourceHealth()[0].state == "missing"; });
  health = runtime->sourceHealth()[0];
  assert(health.reader.epoch == 2 && health.reader.disappearances == 1 && !health.ready);
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 43);
  assert(runtime->sharedPV().fetch()["timeStamp.nanoseconds"].as<int32_t>() == good["timeStamp.nanoseconds"].as<int32_t>());
  // A malformed sample in the replacement epoch cannot restore readiness.
  send("read", "1-2", {{"_", "bad"}});
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 2; });
  assert(!runtime->sourceHealth()[0].ready && runtime->sharedPV().fetch()["value"].as<double>() == 43);
  send("read", "2-3", scalar(7));
  eventually([&] { return runtime->sourceHealth()[0].ready && runtime->sharedPV().fetch()["value"].as<double>() == 7; });
  health = runtime->sourceHealth()[0];
  assert(health.reader.cursor == "2-3" && health.lastValidEpoch == 2 && health.reader.streamResets == 1);

  // Observed retention loss is evidence, not an invented number of missed entries.
  // Force a read transport/authentication failure first; ordinary deferral alone
  // does not promise a continuity check before an active key resumes XREAD.
  control.command<void>("ACL", "SETUSER", "runtime-health", "resetpass", ">expired-test-password");
  control.command<long long>("CLIENT", "KILL", "USER", "runtime-health");
  eventually([&] { return !runtime->sourceHealth()[0].reader.connected; });
  adapter->setDeferReaders(true);
  send("read", "3-0", scalar(8)); send("read", "4-0", scalar(9));
  control.command<long long>("XTRIM", key("read"), "MAXLEN", "=", "1");
  // Leave the paused reader's maximum 8 * 100 ms inspection interval due
  // before it resumes, so the gap is observed before XREAD consumes the tail.
  std::this_thread::sleep_for(1s);
  control.command<void>("ACL", "SETUSER", "runtime-health", "resetpass", "nopass");
  adapter->setDeferReaders(false);
  eventually([&] { return runtime->sourceHealth()[0].reader.retentionGaps > 0; });
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 9; });
  assert(runtime->sourceHealth()[0].reader.epoch == 2);

  // Missing initial data has zero source time and explicit unready state.
  PVConfig absent; absent.name = "absent"; absent.read = {"default", "absent"}; absent.initialValue = -3.;
  auto empty = make(absent); empty->activate();
  eventually([&] { return empty->sourceHealth()[0].reader.connected; });
  assert(!empty->sourceHealth()[0].ready && empty->sourceHealth()[0].lastValidAgeMs == -1);
  assert(empty->sharedPV().fetch()["value"].as<double>() == -3);
  assert(empty->sharedPV().fetch()["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);

  // A shared read/confirmation route owns one reader and has one diagnostic row.
  auto shared = config; shared.name = "shared"; shared.write = RouteConfig{"default", "command"};
  shared.confirm = ConfirmConfig{"default", "read", 250};
  auto joined = make(shared); joined->activate();
  eventually([&] { return joined->sourceHealth()[0].ready; });
  assert(joined->sourceHealth().size() == 1 && joined->sourceHealth()[0].role == "read-confirm");
  shared.name = "isolated"; shared.confirm->key = "ack";
  auto separate = make(shared); separate->activate();
  eventually([&] { return separate->sourceHealth()[1].reader.connected; });
  assert(separate->sourceHealth().size() == 2 && separate->sourceHealth()[0].ready && !separate->sourceHealth()[1].ready);
  send("ack", "1-0", scalar(999));
  eventually([&] { return separate->sourceHealth()[1].ready; });
  assert(separate->sharedPV().fetch()["value"].as<double>() == 9);
  send("ack", "2-0", {{"_", "bad"}});
  eventually([&] { return separate->sourceHealth()[1].invalidSamples == 1; });
  assert(separate->sourceHealth()[0].ready && !separate->sourceHealth()[1].ready);

  // Inspection denied is separate from a working source read. A wrong-type
  // repair still recovers after permission changes, with current-epoch validity.
  control.command<void>("ACL", "SETUSER", "health-reader", "reset", "on", "nopass", "~{health-acl}:*", "+@all", "-xinfo");
  auto deniedOptions = options; deniedOptions.cxn.user = "health-reader";
  auto denied = std::make_shared<RedisAdapter>("health-acl", deniedOptions);
  RedisBackendRegistry deniedBackends{{"default", denied}};
  auto fields = scalar(5);
  control.xadd("{health-acl}:read", "10-0", fields.begin(), fields.end());
  auto deniedRuntime = makeRuntime(server, config, deniedBackends, {}, 1, {}, {}, {}, policies);
  deniedRuntime->activate();
  eventually([&] {
    const auto status = deniedRuntime->sourceHealth()[0];
    return status.inspection == "rejected" && status.ready;
  });
  assert(deniedRuntime->sourceHealth()[0].ready && deniedRuntime->sourceHealth()[0].reader.readFailures == 0);
  control.del("{health-acl}:read"); control.set("{health-acl}:read", "wrong type");
  eventually([&] { return deniedRuntime->sourceHealth()[0].state == "wrong-type"; });
  control.del("{health-acl}:read");
  control.xadd("{health-acl}:read", "1-0", fields.begin(), fields.end());
  eventually([&] { return deniedRuntime->sourceHealth()[0].ready; });
  assert(deniedRuntime->sourceHealth()[0].lastValidEpoch > 1);
  control.command<void>("ACL", "SETUSER", "health-reader", "-xread");
  eventually([&] { return deniedRuntime->sourceHealth()[0].state == "read-rejected"; });
  assert(deniedRuntime->sourceHealth()[0].reader.connected && !deniedRuntime->sourceHealth()[0].ready);
  control.command<void>("ACL", "SETUSER", "health-reader", "+xread");
  control.xadd("{health-acl}:read", "2-0", fields.begin(), fields.end());
  eventually([&] { return deniedRuntime->sourceHealth()[0].ready; });
  deniedRuntime->deactivate("test complete"); deniedRuntime.reset(); denied.reset();
  control.command<long long>("ACL", "DELUSER", "health-reader");

  // A disabled inspection policy remains explicit while normal delivery works.
  policies.at("default").readerProbeMs = 0;
  auto noProbe = make(config); noProbe->activate();
  eventually([&] { return noProbe->sourceHealth()[0].ready; });
  assert(noProbe->sourceHealth()[0].inspection == "disabled");
  assert(!noProbe->sourceHealth()[0].reader.inspected);
  noProbe->deactivate("complete");

  // NTNDArray readiness retains acquisition time and pixels through deletion.
  policies.at("default").readerProbeMs = 100;
  RedisAdapter::Attrs frame{{"schema", kNTNDArrayRedisSchema}, {"data_type", "1"}, {"shape", "[4]"},
      {"color_mode", "mono"}, {"unique_id", "1"}, {"_", std::string(4, char(11))}};
  send("frame", "100-0", frame);
  PVConfig image; image.name = "frame"; image.kind = PVKind::NTNDArray; image.read = {"default", "frame"};
  auto imaging = make(image); imaging->activate();
  eventually([&] { return imaging->sourceHealth()[0].ready; });
  const auto imageBefore = imaging->sharedPV().fetch();
  control.del(key("frame"));
  eventually([&] { return imaging->sourceHealth()[0].state == "missing"; });
  assert(imaging->sharedPV().fetch()["value"].as<pvxs::shared_array<const uint8_t>>()[0] == 11);
  assert(imaging->sharedPV().fetch()["dataTimeStamp.secondsPastEpoch"].as<int64_t>() == imageBefore["dataTimeStamp.secondsPastEpoch"].as<int64_t>());
  assert(imaging->sharedPV().fetch()["dataTimeStamp.nanoseconds"].as<int32_t>() == imageBefore["dataTimeStamp.nanoseconds"].as<int32_t>());
  frame["unique_id"] = "2"; frame["_"] = std::string(4, char(12)); send("frame", "1-0", frame);
  eventually([&] { return imaging->sourceHealth()[0].ready; });
  assert(imaging->sourceHealth()[0].lastValidEpoch == 2 && imaging->sourceHealth()[0].reader.cursor == "1-0");
  assert(imaging->sharedPV().fetch()["uniqueId"].as<int32_t>() == 2);
  runtime->deactivate("complete");
  assert(!runtime->sourceHealth()[0].ready && !runtime->sourceHealth()[0].reader.active);
  std::cout << "source health: exact cursors, validity, freshness, epochs, retention, ACL transitions and imaging passed\n";
}
