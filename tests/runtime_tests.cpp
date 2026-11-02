#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/alarm_publisher.h"
#include "RedisAdapter.hpp"
#include <alarm.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <sys/socket.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class Predicate> void eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  assert(predicate());
}

struct Completion {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  std::string error;
  void complete(pvxs::client::Result&& result) {
    std::string failure;
    try { result(); } catch (const std::exception& ex) { failure = ex.what(); }
    std::lock_guard<std::mutex> guard(mutex);
    error = failure; done = true; changed.notify_all();
  }
  bool waitFor(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, duration, [&] { return done; });
  }
};

void assertTimestamp(const pvxs::Value& value, int64_t seconds, int32_t nanoseconds) {
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == seconds);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == nanoseconds);
  assert(value["timeStamp.userTag"].as<int32_t>() == 0);
}

void assertWaveform(const pvxs::Value& value, const std::vector<float>& expected) {
  assert(value["value"].type().code == pvxs::TypeCode::Float32A);
  const auto actual = value["value"].as<pvxs::shared_array<const float>>();
  assert(actual.size() == expected.size());
  assert(expected.empty() || std::memcmp(actual.data(), expected.data(), expected.size() * sizeof(float)) == 0);
}

RedisAdapter::Attrs waveformFields(const std::vector<float>& waveform,
                                  const std::optional<std::string>& timestamp = {},
                                  const std::optional<std::string>& valid = {}) {
  std::string bytes(waveform.size() * sizeof(float), '\0');
  if (!bytes.empty()) std::memcpy(bytes.data(), waveform.data(), bytes.size());
  RedisAdapter::Attrs fields{{"_", std::move(bytes)}};
  if (timestamp) fields["sample_time_ns"] = *timestamp;
  if (valid) fields["sample_time_valid"] = *valid;
  return fields;
}

void exerciseSourceTime(const ServerConfig& serverConfig, const RedisBackendRegistry& backends,
                        uint16_t redisPort, pvxs::server::Server& server, pvxs::client::Context& client) {
  sw::redis::ConnectionOptions connection;
  connection.host = "127.0.0.1"; connection.port = redisPort;
  sw::redis::Redis control(connection);
  const auto send = [&](const std::string& key, const std::string& id, const RedisAdapter::Attrs& fields) {
    assert(control.xadd("{runtime-tests}:" + key, id, fields.begin(), fields.end()) == id);
  };
  const std::vector<float> first{0.125f, -0.0f, 65536.5f, -12.25f};
  const std::vector<float> second{-9.5f, 0.0f, 1.25f};
  PVConfig config;
  config.name = "source-time"; config.type = PrimitiveType::Float32; config.shape = Shape::Array;
  config.read = {"default", "source-time"}; config.metadata.units = "mm";
  config.sourceTime = {SourceTimeMode::UnixNanosecondsV1, "sample_time_ns", "sample_time_valid"};

  // The transport cursor is deliberately unrelated to acquisition time and
  // carries a sequence beyond exact double precision.
  send(config.read.key, "10-9007199254740993", waveformFields(first, "1700000000123456789", "1"));
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1);
  runtime->activate(); server.addPV(runtime->fullName(), runtime->sharedPV());
  eventually([&] { return runtime->sourceHealth()[0].ready; });
  auto initial = client.get(runtime->fullName()).exec()->wait(3.);
  assertWaveform(initial, first); assertTimestamp(initial, 1700000000, 123456789);
  assert(initial["display.units"].as<std::string>() == "mm");
  assert(initial["alarm.severity"].as<int>() == epicsSevNone);
  auto health = runtime->sourceHealth()[0];
  assert(health.sourceTimeMode == "unix-nanoseconds-v1" && health.sourceTimePresent && health.sourceTimeValid);
  assert(health.sourceTimeError.empty() && health.lastValidCursor == "10-9007199254740993");
  const auto epoch = health.reader.epoch;

  pvxs::shared_array<float> attempted(second.begin(), second.end());
  bool denied = false;
  try { client.put(runtime->fullName()).set("value", attempted.freeze()).exec()->wait(3.); }
  catch (const std::exception& error) { denied = std::string(error.what()).find("Read-only PV") != std::string::npos; }
  assert(denied);
  assertWaveform(runtime->sharedPV().fetch(), first);
  assert(runtime->sourceHealth()[0].lastValidCursor == "10-9007199254740993");
  assert(backends.at("default")->getStreamSnapshot(config.read.key).id == "10-9007199254740993");

  const auto accepted = [&](const std::string& id, const RedisAdapter::Attrs& fields) {
    send(config.read.key, id, fields);
    eventually([&] { return runtime->sourceHealth()[0].lastValidCursor == id; });
  };
  // Epoch zero is supplied source time, not a request to stamp wall time.
  accepted("11-9007199254740994", waveformFields(second, "0", "1"));
  assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  assert(runtime->sourceHealth()[0].sourceTimePresent && runtime->sourceHealth()[0].sourceTimeValid);
  assert(runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);
  auto metadata = config; metadata.metadata.description = "epoch-zero metadata refresh";
  runtime->reconfigure(metadata, 2);
  assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  assertWaveform(runtime->sharedPV().fetch(), second);

  // Acquisition time may move backward, including before the Unix epoch,
  // without rewinding the exact Redis cursor or changing the source epoch.
  accepted("12-9007199254740995", waveformFields(first, "-1", "1"));
  assertTimestamp(runtime->sharedPV().fetch(), -1, 999999999);
  health = runtime->sourceHealth()[0];
  assert(health.ready && health.sourceTimeValid && health.reader.epoch == epoch);
  assert(health.lastValidCursor == "12-9007199254740995");

  // Invalid or absent time still carries useful newly acquired waveform data.
  accepted("13-0", waveformFields(second, "1700000000987654321", "0"));
  auto value = runtime->sharedPV().fetch();
  assertWaveform(value, second); assertTimestamp(value, 1700000000, 987654321);
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid && value["alarm.status"].as<int>() == epicsAlarmUDF);
  health = runtime->sourceHealth()[0];
  assert(health.ready && health.valid && health.fresh && health.sourceTimePresent && !health.sourceTimeValid);
  assert(health.invalidSamples == 0 && !health.sourceTimeError.empty());
  // Snapshot initialization applies the same timestamp-validity contract.
  auto snapshotConfig = config; snapshotConfig.name = "flagged-time-snapshot";
  auto flaggedSnapshot = makeRuntime(serverConfig, snapshotConfig, backends, {}, 1);
  assertWaveform(flaggedSnapshot->sharedPV().fetch(), second);
  assertTimestamp(flaggedSnapshot->sharedPV().fetch(), 1700000000, 987654321);
  assert(flaggedSnapshot->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  flaggedSnapshot->deactivate("snapshot checked");

  accepted("14-0", waveformFields(first, {}, "0"));
  assertWaveform(runtime->sharedPV().fetch(), first); assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  health = runtime->sourceHealth(SourceClock::now() + 24h)[0];
  assert(health.ready && health.valid && !health.sourceTimePresent && !health.sourceTimeValid);
  assert(health.invalidSamples == 0 && !health.sourceTimeError.empty());
  snapshotConfig.name = "unknown-time-snapshot";
  auto unknownSnapshot = makeRuntime(serverConfig, snapshotConfig, backends, {}, 1);
  assertWaveform(unknownSnapshot->sharedPV().fetch(), first);
  assertTimestamp(unknownSnapshot->sharedPV().fetch(), 0, 0);
  assert(!unknownSnapshot->sourceHealth()[0].sourceTimePresent);
  unknownSnapshot->deactivate("snapshot checked");

  accepted("15-0", waveformFields(second, "1700000000000000007"));
  assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 7);
  health = runtime->sourceHealth()[0];
  assert(health.ready && health.sourceTimePresent && !health.sourceTimeValid && health.invalidSamples == 0);

  // Malformed time rejects the whole entry and retains both the previous
  // accepted data and exact timestamp until a later acceptable sample.
  send(config.read.key, "16-0", waveformFields(first, "1e9", "1"));
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 1; });
  health = runtime->sourceHealth()[0];
  assert(!health.ready && !health.valid && health.lastValidCursor == "15-0");
  assert(health.sourceTimePresent && !health.sourceTimeValid && !health.sourceTimeError.empty());
  assertWaveform(runtime->sharedPV().fetch(), second); assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 7);
  accepted("17-0", waveformFields(first, "1700000000123456791", "1"));
  assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 123456791);
  assert(runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);

  auto badPayload = waveformFields(second, "1800000000000000000", "1"); badPayload["_"] = "x";
  send(config.read.key, "18-0", badPayload);
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 2; });
  assertWaveform(runtime->sharedPV().fetch(), first); assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 123456791);
  health = runtime->sourceHealth()[0];
  assert(!health.ready && health.sourceTimePresent && health.sourceTimeValid); // Retained time quality is independent of payload rejection.
  accepted("19-0", waveformFields(second));
  assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  assert(runtime->sourceHealth()[0].ready && !runtime->sourceHealth()[0].sourceTimePresent);

  send(config.read.key, "20-0", waveformFields(first, "123", "true"));
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 3; });
  assertWaveform(runtime->sharedPV().fetch(), second); assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  assert(runtime->sourceHealth()[0].sourceTimeError.find("validity") != std::string::npos);
  send(config.read.key, "21-0", waveformFields(first, {}, "1"));
  eventually([&] { return runtime->sourceHealth()[0].invalidSamples == 4; });
  assertWaveform(runtime->sharedPV().fetch(), second); assertTimestamp(runtime->sharedPV().fetch(), 0, 0);
  accepted("22-0", waveformFields(first, "1700000000000000009", "1"));

  // Staged metadata must refresh the newest accepted sample. Time-policy
  // changes require runtime replacement and cannot mutate the active owner.
  metadata.metadata.description = "staged before newest acquisition";
  auto stagedMetadata = runtime->prepareReconfigure(metadata, 3);
  accepted("23-0", waveformFields(second, "1700000000000000011", "1"));
  stagedMetadata->commit(); stagedMetadata->refresh();
  assertWaveform(runtime->sharedPV().fetch(), second); assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 11);
  auto unavailable = metadata; unavailable.sourceTime = {SourceTimeMode::Unavailable, {}, {}};
  assert(!runtime->structurallyCompatible(unavailable));
  bool reconfigureRejected = false;
  try { runtime->prepareReconfigure(unavailable, 4); }
  catch (const std::exception&) { reconfigureRejected = true; }
  assert(reconfigureRejected);
  auto stagedReplacement = makeRuntime(serverConfig, unavailable, backends, {}, 4);
  assertWaveform(stagedReplacement->sharedPV().fetch(), second);
  assertTimestamp(stagedReplacement->sharedPV().fetch(), 0, 0);
  stagedReplacement->activate();
  eventually([&] { return stagedReplacement->sourceHealth()[0].ready; });
  const auto unavailableHealth = stagedReplacement->sourceHealth()[0];
  assert(unavailableHealth.sourceTimeMode == "unavailable" && !unavailableHealth.sourceTimePresent && !unavailableHealth.sourceTimeValid);
  assert(stagedReplacement->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  assertTimestamp(runtime->sharedPV().fetch(), 1700000000, 11);
  stagedReplacement->deactivate("staging rejected");
  accepted("24-0", waveformFields(first, "1700000000000000013", "1"));
  assert(runtime->sourceHealth()[0].reader.epoch == epoch && runtime->sourceHealth()[0].sourceTimeValid);

  // A malformed initial timestamp exposes invalid fallback with no fabricated
  // source time. Later valid input recovers that same owned subscription.
  auto malformedConfig = config; malformedConfig.name = "malformed-time-snapshot";
  malformedConfig.read.key = malformedConfig.name; malformedConfig.initialValue = second;
  send(malformedConfig.read.key, "1-0", waveformFields(first, "9223372036854775808", "1"));
  auto malformed = makeRuntime(serverConfig, malformedConfig, backends, {}, 1);
  assertWaveform(malformed->sharedPV().fetch(), second); assertTimestamp(malformed->sharedPV().fetch(), 0, 0);
  assert(malformed->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(!malformed->sourceHealth()[0].sourceTimePresent && !malformed->sourceHealth()[0].sourceTimeError.empty());
  malformed->activate();
  send(malformedConfig.read.key, "2-0", waveformFields(first, "1700000000000000015", "1"));
  eventually([&] { return malformed->sourceHealth()[0].ready; });
  assertWaveform(malformed->sharedPV().fetch(), first); assertTimestamp(malformed->sharedPV().fetch(), 1700000000, 15);
  malformed->deactivate("done");
  server.removePV(runtime->fullName()); runtime->deactivate("done");
}

void exerciseConfirmationTime(const ServerConfig& serverConfig, const RedisBackendRegistry& backends,
                              uint16_t redisPort) {
  sw::redis::ConnectionOptions connection;
  connection.host = "127.0.0.1"; connection.port = redisPort;
  sw::redis::Redis control(connection);
  const double readback = 8.0;
  std::string bytes(sizeof(readback), '\0');
  std::memcpy(bytes.data(), &readback, sizeof(readback));
  const RedisAdapter::Attrs fields{{"_", bytes}};
  const std::string ackKey = "{runtime-tests}:confirmation-time-ack";
  assert(control.xadd("{runtime-tests}:confirmation-time-read", "10-37", fields.begin(), fields.end()) == "10-37");
  assert(control.xadd(ackKey, "1-0", fields.begin(), fields.end()) == "1-0");
  PVConfig config;
  config.name = "confirmation-time"; config.read = {"default", "confirmation-time-read"};
  config.write = RouteConfig{"default", "confirmation-time-command"};
  config.confirm = ConfirmConfig{"default", "confirmation-time-ack", 250};
  RedisBackendConfigs policies{{"default", RedisConfig{}}};
  policies.at("default").readerProbeMs = 100;
  auto runtime = makeRuntime(serverConfig, config, backends, {}, 1, {}, {}, {}, policies);
  runtime->activate();
  eventually([&] { const auto status = runtime->sourceHealth(); return status[0].ready && status[1].ready; });
  auto confirm = runtime->sourceHealth()[1];
  assert(confirm.sourceTimePresent && confirm.sourceTimeValid && confirm.sourceTimeError.empty());
  const auto epoch = confirm.reader.epoch;
  const auto assertReadback = [&] {
    assert(runtime->sharedPV().fetch()["value"].as<double>() == readback);
    assertTimestamp(runtime->sharedPV().fetch(), 0, 10000037);
    const auto read = runtime->sourceHealth()[0];
    assert(read.ready && read.sourceTimePresent && read.sourceTimeValid && read.sourceTimeError.empty());
  };

  // Payload failure retains the accepted confirmation time quality. A valid
  // Redis ID outside legacy int64 time range must report a time failure.
  const RedisAdapter::Attrs malformed{{"_", "x"}};
  assert(control.xadd(ackKey, "2-0", malformed.begin(), malformed.end()) == "2-0");
  eventually([&] { return runtime->sourceHealth()[1].invalidSamples == 1; });
  confirm = runtime->sourceHealth()[1];
  assert(!confirm.ready && confirm.sourceTimePresent && confirm.sourceTimeValid && confirm.sourceTimeError.empty());
  assertReadback();
  assert(control.xadd(ackKey, "9223372036855-0", fields.begin(), fields.end()) == "9223372036855-0");
  eventually([&] { return runtime->sourceHealth()[1].invalidSamples == 2; });
  confirm = runtime->sourceHealth()[1];
  assert(!confirm.ready && confirm.lastValidCursor == "1-0" && confirm.sourceTimePresent && !confirm.sourceTimeValid);
  assert(confirm.sourceTimeError.find("legacy Redis source timestamp") != std::string::npos);
  assertReadback();

  auto snapshotConfig = config; snapshotConfig.name = "confirmation-time-snapshot";
  auto snapshot = makeRuntime(serverConfig, snapshotConfig, backends, {}, 1, {}, {}, {}, policies);
  const auto invalidSnapshot = snapshot->sourceHealth()[1];
  assert(!invalidSnapshot.sourceTimePresent && !invalidSnapshot.sourceTimeValid);
  assert(invalidSnapshot.sourceTimeError.find("legacy Redis source timestamp") != std::string::npos);
  snapshot->deactivate("snapshot checked");

  // Recreate this isolated fixture key to recover below the oversized cursor.
  assert(control.del(ackKey) == 1);
  eventually([&] { return runtime->sourceHealth()[1].state == "missing"; });
  assert(control.xadd(ackKey, "1-0", fields.begin(), fields.end()) == "1-0");
  eventually([&] { const auto status = runtime->sourceHealth()[1]; return status.ready && status.lastValidEpoch > epoch; });
  confirm = runtime->sourceHealth()[1];
  assert(confirm.sourceTimePresent && confirm.sourceTimeValid && confirm.sourceTimeError.empty());
  assertReadback(); runtime->deactivate("done");
}

int main() {
  auto server = pvxs::server::Config::isolated(AF_INET).build();
  RA_Options options;
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  options.cxn.port = std::stoi(port);
  auto adapter = std::make_shared<RedisAdapter>("runtime-tests", options);
  RedisAdapter producer("runtime-tests", options);
  assert(adapter->connected());
  RedisBackendRegistry backends{{"default", adapter}};
  ServerConfig serverConfig;
  serverConfig.instance = "runtime-tests";
  serverConfig.nameSpace = "TEST";
  PVConfig config;
  config.name = "value";
  config.read = {"default", "readback"};
  config.write = RouteConfig{"default", "command"};
  config.confirm = ConfirmConfig{"default", "ack", 1500};
  assert(producer.addSingleDouble("readback", 0.).ok());
  auto alarms = std::make_shared<AlarmPublisher>("127.0.0.1", options.cxn.port, "runtime-test-alarms");
  alarms->activate();
  auto runtime = makeRuntime(serverConfig, config, backends, alarms, 1);
  assert(alarms->status().active == 0 && alarms->status().sent == 0);
  runtime->activate();
  eventually([&] { return alarms->status().state == "ready"; });
  server.addPV(runtime->fullName(), runtime->sharedPV()).start();
  auto client = server.clientConfig().build();
  assert(client.get("TEST:value").exec()->wait(3.)["value"].as<double>() == 0.);

  Completion completion;
  auto operation = client.put("TEST:value").set("value", 123.)
      .result([&](pvxs::client::Result&& result) { completion.complete(std::move(result)); }).exec();
  eventually([&] {
    double value = 0.;
    return producer.getSingleValue<double>("command", value).ok() && value == 123.;
  });
  // A pending confirmation must not block unrelated PVA operations.
  assert(client.get("TEST:value").exec()->wait(.5)["value"].as<double>() == 0.);
  const auto readbackTime = producer.addSingleDouble("readback", 123.); assert(readbackTime.ok());
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 123.; });
  assert(!completion.waitFor(100ms));
  assert(producer.addSingleDouble("ack", 123.).ok());
  assert(completion.waitFor(3s) && completion.error.empty());
  assertTimestamp(runtime->sharedPV().fetch(), readbackTime.value / 1000000000, readbackTime.value % 1000000000);
  assert(runtime->sourceHealth()[0].sourceTimeMode == "legacy-redis-id");
  assert(runtime->sourceHealth()[0].sourceTimeValid && runtime->sourceHealth()[1].sourceTimeValid);

  // The second subscriber runs after the runtime's confirmation subscriber.
  std::mutex ackMutex;
  std::condition_variable ackChanged;
  std::string ackId;
  auto observer = adapter->subscribeStream("ack", [&](const auto&, const auto&, const auto& batch) {
    std::lock_guard<std::mutex> guard(ackMutex);
    ackId = batch.back().first; ackChanged.notify_all();
  });
  auto otherAck = producer.addSingleDouble("ack", 456.);
  {
    std::unique_lock<std::mutex> lock(ackMutex);
    assert(ackChanged.wait_for(lock, 3s, [&] { return ackId == otherAck.id(); }));
  }
  auto metadata = config;
  metadata.metadata.description = "new description";
  runtime->reconfigure(metadata, 2);
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 123.);
  assert(runtime->sharedPV().fetch()["display.description"].as<std::string>() == "new description");

  // A matching observation that predates the command, and metadata replay of
  // cached readback, must not satisfy a new confirmed write.
  auto staleAck = producer.addSingleDouble("ack", 123.);
  {
    std::unique_lock<std::mutex> lock(ackMutex);
    assert(ackChanged.wait_for(lock, 3s, [&] { return ackId == staleAck.id(); }));
  }
  auto previousCommand = producer.getStreamSnapshot("command").id;
  Completion repeated;
  auto repeatedOperation = client.put("TEST:value").set("value", 123.)
      .result([&](pvxs::client::Result&& result) { repeated.complete(std::move(result)); }).exec();
  eventually([&] { return producer.getStreamSnapshot("command").id != previousCommand; });
  metadata.metadata.description = "metadata during pending write";
  runtime->reconfigure(metadata, 3);
  auto rejectedConfig = metadata;
  rejectedConfig.transform = LinearTransformConfig{2., 0.};
  {
    auto rejected = runtime->prepareReconfigure(rejectedConfig, 4);
    assert(runtime->sharedPV().fetch()["value"].as<double>() == 123.);
  }
  assert(!repeated.waitFor(100ms));
  assert(producer.addSingleDouble("ack", 123.).ok());
  assert(repeated.waitFor(3s) && repeated.error.empty());

  // Malformed source input keeps the last value and marks it invalid, then recovers.
  assert(producer.addSingleValue<RedisAdapter::Attrs>("readback", {{"_", "x"}}).ok());
  eventually([&] { return runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid; });
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 123.);
  assert(producer.addSingleDouble("readback", 7.).ok());
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 7.; });
  assert(runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);

  auto stagedMetadata = metadata;
  stagedMetadata.metadata.description = "prepared before the newest source sample";
  stagedMetadata.alarms.highWarning = 7.25;
  auto nextAlarms = std::make_shared<AlarmPublisher>("127.0.0.1", options.cxn.port, "{runtime-tests}:alarm-next");
  auto stagedUpdate = runtime->prepareReconfigure(stagedMetadata, 4, nextAlarms);
  assert(producer.addSingleDouble("readback", 7.5).ok());
  eventually([&] { return runtime->sharedPV().fetch()["value"].as<double>() == 7.5; });
  assert(nextAlarms->status().active == 0 && nextAlarms->status().sent == 0);
  stagedUpdate->commit();
  alarms->stop();
  stagedUpdate->refresh();
  nextAlarms->activate();
  eventually([&] { return nextAlarms->status().state == "ready"; });
  assert(nextAlarms->status().active == 1 && nextAlarms->status().sent == 1);
  assert(producer.getStreamSnapshot("alarm-next").fields.at("severity") == "MINOR");
  assert(runtime->sharedPV().fetch()["value"].as<double>() == 7.5);

  observer.reset();
  auto replacementConfig = config;
  replacementConfig.confirm.reset();
  replacementConfig.write = RouteConfig{"default", "new-command"};
  adapter->setDeferReaders(true);
  auto replacement = makeRuntime(serverConfig, replacementConfig, backends, {}, 3);
  adapter->setDeferReaders(false);
  server.removePV(runtime->fullName());
  runtime->deactivate("replaced");
  replacement->activate();
  server.addPV(replacement->fullName(), replacement->sharedPV());
  assert(producer.addSingleDouble("readback", 8.).ok());
  eventually([&] { return replacement->sharedPV().fetch()["value"].as<double>() == 8.; });
  {
    auto rejected = makeRuntime(serverConfig, replacementConfig, backends, {}, 4);
    rejected->deactivate("staging rejected");
  }
  assert(producer.addSingleDouble("readback", 9.).ok());
  eventually([&] { return replacement->sharedPV().fetch()["value"].as<double>() == 9.; });

  PVConfig gapConfig;
  gapConfig.name = "gap";
  gapConfig.read = {"default", "gap"};
  assert(producer.addSingleDouble("gap", 1.).ok());
  adapter->setDeferReaders(true);
  auto gap = makeRuntime(serverConfig, gapConfig, backends, {}, 1);
  assert(producer.addSingleDouble("gap", 2.).ok());
  adapter->setDeferReaders(false);
  gap->activate();
  eventually([&] { return gap->sharedPV().fetch()["value"].as<double>() == 2.; });

  PVConfig emptyConfig;
  emptyConfig.name = "empty";
  emptyConfig.type = PrimitiveType::Float64;
  emptyConfig.shape = Shape::Array;
  emptyConfig.read = {"default", "empty"};
  assert(producer.addSingleList<double>("empty", std::vector<double>{}).ok());
  auto empty = makeRuntime(serverConfig, emptyConfig, backends, {}, 1);
  assert(empty->sharedPV().fetch()["value"].as<pvxs::shared_array<const double>>().empty());
  assert(empty->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);

  PVConfig missingConfig;
  missingConfig.name = "missing";
  missingConfig.read = {"default", "missing"};
  missingConfig.initialValue = 10.;
  auto missing = makeRuntime(serverConfig, missingConfig, backends, {}, 1);
  assert(missing->sharedPV().fetch()["value"].as<double>() == 10.);
  assert(missing->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(missing->sharedPV().fetch()["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);

  exerciseSourceTime(serverConfig, backends, options.cxn.port, server, client);
  exerciseConfirmationTime(serverConfig, backends, options.cxn.port);

  missing->deactivate("done"); empty->deactivate("done"); gap->deactivate("done");
  client.close(); server.stop(); replacement->deactivate("done");
  std::cout << "runtime read/confirm isolation, source-time policy, exact waveform/time and snapshot tests passed\n";
}
