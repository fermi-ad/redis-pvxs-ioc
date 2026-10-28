#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/xrm_contract.h"
#include "RedisAdapter.hpp"

#include <alarm.h>
#include <pvxs/client.h>
#include <pvxs/server.h>
#include <pvxs/sharedArray.h>
#include <sw/redis++/redis++.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <sys/socket.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<class Predicate> void eventuallyImpl(int line, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  if (!predicate()) std::cerr << "eventually timed out at line " << line << '\n';
  assert(predicate());
}
#define eventually(...) eventuallyImpl(__LINE__, __VA_ARGS__)

RedisAdapter::Attrs waveform(std::string bytes, const std::string& width = "2") {
  const auto count = bytes.size() / std::stoul(width);
  return {{"schema", "fermi-ad/xrm-waveform:1"}, {"sample_bytes", width},
          {"sample_count", std::to_string(count)}, {"_", std::move(bytes)},
          {"session", "session-a"}, {"connection", "1"}, {"batch", "1"},
          {"channel", "1"}, {"sample_rate_hz", "1000000"},
          {"source_validity", "valid"}, {"acq_time_valid", "0"}};
}

std::string samples16(int16_t first, int16_t second) {
  std::string result;
  for (const auto sample : {first, second}) {
    const auto bits = static_cast<uint16_t>(sample);
    result.push_back(static_cast<char>(bits & 0xff));
    result.push_back(static_cast<char>(bits >> 8));
  }
  return result;
}

template<class T> bool equals(const pvxs::Value& value, const std::vector<T>& expected) {
  const auto array = value["value"].as<pvxs::shared_array<const T>>();
  return array.size() == expected.size() && std::equal(array.begin(), array.end(), expected.begin());
}

int main() {
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  RA_Options options;
  options.cxn.port = std::stoi(port);
  auto adapter = std::make_shared<RedisAdapter>("xrm-runtime-tests", options);
  RedisAdapter producer("xrm-runtime-tests", options);
  assert(adapter->connected());
  RedisBackendRegistry backends{{"default", adapter}};
  RedisConfig backend;
  backend.baseKey = "xrm-runtime-tests";
  backend.port = static_cast<uint16_t>(options.cxn.port);
  sw::redis::ConnectionOptions redisOptions;
  redisOptions.host = "127.0.0.1"; redisOptions.port = options.cxn.port; redisOptions.socket_timeout = 10s;
  sw::redis::Redis redis(redisOptions);
  constexpr const char* fixturePublish = R"lua(
local f = {}
for i=2,#ARGV do f[#f+1] = ARGV[i] end
local id = redis.call('XADD', KEYS[1], '*', unpack(f))
redis.call('DEL', KEYS[2])
redis.call('HSET', KEYS[2], 'contract', 'fermi-ad/xrm-latest:1', 'state', 'ready', 'commit', ARGV[1], 'stream_id', id, unpack(f))
redis.call('SET', KEYS[3], ARGV[1])
return id
)lua";
  auto publishXrm = [&](const std::string& key, const RedisAdapter::Attrs& fields) {
    const auto stream = "{xrm-runtime-tests}:" + key;
    const auto field = [&](const char* name) {
      const auto found = fields.find(name);
      return found == fields.end() ? std::string{} : found->second;
    };
    const auto token = field("session") + "/" + field("connection") + "/" + field("batch");
    std::vector<std::string> args{"EVAL", fixturePublish, "3", stream, xrmLatestHashKey(stream),
                                  xrmCommitMarkerKey(stream), token};
    for (const auto& field : fields) { args.push_back(field.first); args.push_back(field.second); }
    const auto reply = redis.command(args.begin(), args.end());
    assert(reply && reply->type == REDIS_REPLY_STRING);
    return RA_Time(std::string(reply->str, reply->len));
  };
  RedisBackendConfigs readerPolicies{{"default", backend}};
  ServerConfig server;
  server.instance = "xrm-runtime-tests";
  server.nameSpace = "TEST";
  PVConfig config;
  config.name = "waveform";
  config.type = PrimitiveType::Int16;
  config.shape = Shape::Array;
  config.read = {"default", "waveform"};
  config.streamProtocol = StreamProtocol::XrmV1;
  config.sourceHealth.staleAfterMs = 5000;
  const auto make = [&](const PVConfig& pv) {
    return makeRuntime(server, pv, backends, {}, 1, {}, {}, {}, readerPolicies);
  };

  // A startup snapshot without a proven device epoch still exposes samples,
  // but never substitutes the Redis ID or receipt time as acquisition time.
  auto fields = waveform(samples16(-2, 0x1234));
  fields["source_validity"] = "unknown";
  assert(publishXrm("waveform", fields).ok());
  auto runtime = make(config);
  runtime->activate();
  auto initial = runtime->sharedPV().fetch();
  assert(equals<int16_t>(initial, {-2, 0x1234}));
  assert(initial["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);
  assert(initial["timeStamp.nanoseconds"].as<int32_t>() == 0);
  assert(initial["alarm.severity"].as<int>() == epicsSevInvalid);
  const auto initialMessage = initial["alarm.message"].as<std::string>();
  assert(initialMessage.find("acquisition time") != std::string::npos);
  assert(initialMessage.find("source validity") != std::string::npos);
  assert(initial["xrm.present"].as<bool>());
  assert(initial["xrm.session"].as<std::string>() == "session-a");
  assert(initial["xrm.connection"].as<uint64_t>() == 1);
  assert(initial["xrm.batch"].as<uint64_t>() == 1);
  assert(initial["xrm.channel"].as<uint32_t>() == 1);
  assert(initial["xrm.sampleRateHz"].as<uint64_t>() == 1000000);
  assert(initial["xrm.sampleCount"].as<uint64_t>() == 2);
  assert(initial["xrm.sampleBytes"].as<uint32_t>() == 2);
  assert(initial["xrm.sourceValidity"].as<std::string>() == "unknown");
  assert(!initial["xrm.acqTimeValid"].as<bool>());
  assert(!initial["xrm.acqTimePresent"].as<bool>());
  assert(!initial["xrm.acqTimeParsed"].as<bool>());
  const auto initialHealth = runtime->sourceHealth();
  assert(initialHealth.size() == 1 && !initialHealth[0].ready && !initialHealth[0].valid);
  assert(initialHealth[0].reader.lastReceived != SourceClock::time_point{});

  uint64_t batch = 1;
  auto publish16 = [&](int16_t first, int16_t second, const std::string& valid, const std::string& time) {
    auto next = waveform(samples16(first, second));
    next["batch"] = std::to_string(++batch);
    next["acq_time_valid"] = valid;
    if (!time.empty()) next["acq_time_ns"] = time;
    const auto id = publishXrm("waveform", next);
    assert(id.ok());
    eventually([&] { return runtime->sharedPV().fetch()["xrm.batch"].as<uint64_t>() == batch; });
    return id;
  };
  const auto validId = publish16(1, 2, "1", "1700000000123456789");
  auto value = runtime->sharedPV().fetch();
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1700000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 123456789);
  assert(value["alarm.severity"].as<int>() == epicsSevNone);
  assert(value["xrm.batch"].as<uint64_t>() == 2);
  assert(value["xrm.acqTimeValid"].as<bool>());
  assert(value["xrm.acqTimePresent"].as<bool>() && value["xrm.acqTimeParsed"].as<bool>());

  // Cursor order, rather than device timestamp order, determines delivery.
  const auto duplicateId = publish16(3, 4, "1", "1700000000123456789");
  assert(RedisAdapter::compareStreamIds(duplicateId.id(), validId.id()) > 0);
  value = runtime->sharedPV().fetch();
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1700000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 123456789);
  const auto backwardId = publish16(5, 6, "1", "1600000000000000042");
  assert(RedisAdapter::compareStreamIds(backwardId.id(), duplicateId.id()) > 0);
  value = runtime->sharedPV().fetch();
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 42);
  assert(value["alarm.severity"].as<int>() == epicsSevNone);

  // Invalid but parseable device times remain visible with an invalid alarm;
  // an absent or unparseable time is zero, including after a good sample.
  for (const auto& test : std::vector<std::tuple<std::string, std::string, int64_t, int32_t>>{
           {"0", "", 0, 0}, {"0", "1700000000000000003", 1700000000, 3},
           {"1", "", 0, 0}, {"1", "-1", 0, 0}, {"1", "12x", 0, 0},
           {"1", "18446744073709551616", 0, 0}}) {
    static int16_t sequence = 10;
    publish16(sequence, sequence + 1, std::get<0>(test), std::get<1>(test));
    sequence += 2;
    value = runtime->sharedPV().fetch();
    assert(value["alarm.severity"].as<int>() == epicsSevInvalid);
    assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == std::get<2>(test));
    assert(value["timeStamp.nanoseconds"].as<int32_t>() == std::get<3>(test));
    assert(value["xrm.acqTimeValid"].as<bool>() == (std::get<0>(test) == "1"));
    assert(value["xrm.acqTimePresent"].as<bool>() == !std::get<1>(test).empty());
    assert(value["xrm.acqTimeParsed"].as<bool>() == (std::get<2>(test) != 0));
  }
  const auto health = runtime->sourceHealth();
  assert(health.size() == 1 && !health[0].valid && health[0].state == "invalid");
  assert(health[0].lastValidCursor == backwardId.id());

  publish16(40, 41, "1", "1600000000000000001");
  value = runtime->sharedPV().fetch();
  assert(value["alarm.severity"].as<int>() == epicsSevNone);
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 1);

  auto invalidSource = waveform(samples16(42, 43));
  invalidSource["source_validity"] = "invalid";
  invalidSource["acq_time_valid"] = "1";
  invalidSource["acq_time_ns"] = "1600000000000000002";
  assert(publishXrm("waveform", invalidSource).ok());
  eventually([&] { return equals<int16_t>(runtime->sharedPV().fetch(), {42, 43}); });
  value = runtime->sharedPV().fetch();
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 2);
  assert(value["xrm.sourceValidity"].as<std::string>() == "invalid");
  assert(value["xrm.acqTimeValid"].as<bool>());
  auto unknownSource = waveform(samples16(44, 45));
  unknownSource["source_validity"] = "unknown";
  unknownSource["acq_time_valid"] = "1";
  unknownSource["acq_time_ns"] = "1600000000000000003";
  assert(publishXrm("waveform", unknownSource).ok());
  eventually([&] { return equals<int16_t>(runtime->sharedPV().fetch(), {44, 45}); });
  value = runtime->sharedPV().fetch();
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(value["xrm.sourceValidity"].as<std::string>() == "unknown");
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 3);
  auto bothInvalid = waveform(samples16(46, 47));
  bothInvalid["source_validity"] = "invalid";
  bothInvalid["acq_time_valid"] = "0";
  bothInvalid["acq_time_ns"] = "1600000000000000004";
  assert(publishXrm("waveform", bothInvalid).ok());
  eventually([&] { return equals<int16_t>(runtime->sharedPV().fetch(), {46, 47}); });
  value = runtime->sharedPV().fetch();
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid);
  const auto bothMessage = value["alarm.message"].as<std::string>();
  assert(bothMessage.find("acquisition time") != std::string::npos);
  assert(bothMessage.find("invalid waveform") != std::string::npos);
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 4);
  assert(!value["xrm.acqTimeValid"].as<bool>());
  publish16(40, 41, "1", "1600000000000000001");
  eventually([&] { return runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone; });

  // Unsupported width, malformed schema, and partial samples retain the last
  // waveform and timestamp while changing the alarm to invalid.
  std::vector<RedisAdapter::Attrs> bad;
  auto wrongWidth = waveform(std::string("abcdef", 6), "3");
  bad.push_back(wrongWidth);
  auto wrongSchema = waveform(samples16(50, 51));
  wrongSchema["schema"] = "fermi-ad/xrm-waveform:2";
  bad.push_back(wrongSchema);
  auto badSourceValidity = waveform(samples16(50, 51));
  badSourceValidity["source_validity"] = "perhaps";
  bad.push_back(badSourceValidity);
  auto missingSourceValidity = waveform(samples16(50, 51));
  missingSourceValidity.erase("source_validity");
  bad.push_back(missingSourceValidity);
  auto wrongCount = waveform(samples16(50, 51));
  wrongCount["sample_count"] = "1";
  bad.push_back(wrongCount);
  auto missingCount = waveform(samples16(50, 51));
  missingCount.erase("sample_count");
  bad.push_back(missingCount);
  auto missingSession = waveform(samples16(50, 51));
  missingSession.erase("session");
  bad.push_back(missingSession);
  auto malformedSession = waveform(samples16(50, 51));
  malformedSession["session"] = std::string(129, 'x');
  bad.push_back(malformedSession);
  auto badConnection = waveform(samples16(50, 51));
  badConnection["connection"] = "0";
  bad.push_back(badConnection);
  auto badBatch = waveform(samples16(50, 51));
  badBatch["batch"] = "abc";
  bad.push_back(badBatch);
  auto badChannel = waveform(samples16(50, 51));
  badChannel["channel"] = "33";
  bad.push_back(badChannel);
  auto badRate = waveform(samples16(50, 51));
  badRate["sample_rate_hz"] = "100000001";
  bad.push_back(badRate);
  auto badTimeFlag = waveform(samples16(50, 51));
  badTimeFlag["acq_time_valid"] = "2";
  bad.push_back(badTimeFlag);
  auto missingTimeFlag = waveform(samples16(50, 51));
  missingTimeFlag.erase("acq_time_valid");
  bad.push_back(missingTimeFlag);
  bad.push_back(waveform(std::string("x", 1)));
  bad.push_back(waveform(""));
  for (const auto& malformed : bad) {
    const auto retained = runtime->sharedPV().fetch();
    const auto retainedBatch = retained["xrm.batch"].as<uint64_t>();
    assert(publishXrm("waveform", malformed).ok());
    eventually([&] { return runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid; });
    value = runtime->sharedPV().fetch();
    assert(equals<int16_t>(value, {40, 41}));
    assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 1600000000);
    assert(value["timeStamp.nanoseconds"].as<int32_t>() == 1);
    assert(value["xrm.session"].as<std::string>() == "session-a");
    assert(value["xrm.batch"].as<uint64_t>() == retainedBatch);
    assert(value["xrm.sourceValidity"].as<std::string>() == "valid");
    // Ensure the next malformed sample is independently observed.
    publish16(40, 41, "1", "1600000000000000001");
    if (runtime->sharedPV().fetch()["alarm.severity"].as<int>() != epicsSevNone)
      publish16(40, 41, "1", "1600000000000000001");
    eventually([&] { return runtime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone; });
  }

  // Consumers can correlate channels and recognize a partial publication by
  // comparing producer session, connection and batch on the waveform value.
  auto channel1 = waveform(samples16(70, 71));
  channel1["session"] = "run-7";
  channel1["connection"] = "4";
  channel1["batch"] = "100";
  channel1["acq_time_valid"] = "1";
  channel1["acq_time_ns"] = "1600000000000000000";
  auto channel2 = channel1;
  channel2["channel"] = "2";
  channel2["_"] = samples16(80, 81);
  assert(publishXrm("XRM:CH1", channel1).ok());
  assert(publishXrm("XRM:CH2", channel2).ok());
  auto channelConfig1 = config;
  channelConfig1.name = "channel1";
  channelConfig1.read.key = "XRM:CH1";
  auto channelConfig2 = config;
  channelConfig2.name = "channel2";
  channelConfig2.read.key = "XRM:CH2";
  auto channelRuntime1 = make(channelConfig1);
  auto channelRuntime2 = make(channelConfig2);
  channelRuntime1->activate(); channelRuntime2->activate();
  auto sample1 = channelRuntime1->sharedPV().fetch();
  auto sample2 = channelRuntime2->sharedPV().fetch();
  assert(sample1["xrm.session"].as<std::string>() == "run-7");
  assert(sample1["xrm.session"].as<std::string>() == sample2["xrm.session"].as<std::string>());
  assert(sample1["xrm.connection"].as<uint64_t>() == sample2["xrm.connection"].as<uint64_t>());
  assert(sample1["xrm.batch"].as<uint64_t>() == sample2["xrm.batch"].as<uint64_t>());
  assert(sample1["xrm.channel"].as<uint32_t>() == 1 && sample2["xrm.channel"].as<uint32_t>() == 2);
  channel1["batch"] = "101";
  channel1["sample_rate_hz"] = "2000000";
  channel1["_"] = samples16(72, 73);
  assert(publishXrm("XRM:CH1", channel1).ok());
  eventually([&] { return equals<int16_t>(channelRuntime1->sharedPV().fetch(), {72, 73}); });
  sample1 = channelRuntime1->sharedPV().fetch();
  sample2 = channelRuntime2->sharedPV().fetch();
  assert(sample1["xrm.batch"].as<uint64_t>() == 101);
  assert(sample1["xrm.sampleRateHz"].as<uint64_t>() == 2000000);
  assert(sample2["xrm.batch"].as<uint64_t>() == 100);
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == 0);
  assert(sample1["xrm.discontinuities"].as<uint64_t>() == 0);
  auto wrongChannel = channel1;
  wrongChannel["channel"] = "2";
  wrongChannel["batch"] = "102";
  wrongChannel["_"] = samples16(90, 91);
  assert(publishXrm("XRM:CH1", wrongChannel).ok());
  eventually([&] { return channelRuntime1->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid; });
  sample1 = channelRuntime1->sharedPV().fetch();
  assert(equals<int16_t>(sample1, {72, 73}));
  assert(sample1["xrm.batch"].as<uint64_t>() == 101 && sample1["xrm.channel"].as<uint32_t>() == 1);
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == 0);

  // A missing/rejected or trimmed batch counts as a numeric gap. Reconnects,
  // new sessions and backward sequence numbers instead count discontinuities,
  // because their missing sample counts cannot be inferred.
  auto advanceChannel = [&](int16_t first, const std::string& session, const std::string& connection,
                            const std::string& number) {
    channel1["session"] = session;
    channel1["connection"] = connection;
    channel1["batch"] = number;
    channel1["_"] = samples16(first, first + 1);
    assert(publishXrm("XRM:CH1", channel1).ok());
    eventually([&] { return equals<int16_t>(channelRuntime1->sharedPV().fetch(), {first, static_cast<int16_t>(first + 1)}); });
    return channelRuntime1->sharedPV().fetch();
  };
  sample1 = advanceChannel(100, "run-7", "4", "104");
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == 2);
  assert(sample1["xrm.discontinuities"].as<uint64_t>() == 0);
  sample1 = advanceChannel(102, "run-7", "4", "105");
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == 2);
  sample1 = advanceChannel(104, "run-7", "5", "106");
  assert(sample1["xrm.connectionChanges"].as<uint64_t>() == 1);
  assert(sample1["xrm.discontinuities"].as<uint64_t>() == 1);
  sample1 = advanceChannel(106, "run-7", "5", "2");
  assert(sample1["xrm.sequenceDiscontinuities"].as<uint64_t>() == 1);
  assert(sample1["xrm.discontinuities"].as<uint64_t>() == 2);
  sample1 = advanceChannel(108, "run-8", "1", "1");
  assert(sample1["xrm.sessionChanges"].as<uint64_t>() == 1);
  assert(sample1["xrm.discontinuities"].as<uint64_t>() == 3);
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == 2);
  sample1 = advanceChannel(110, "run-8", "1", "18446744073709551615");
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == UINT64_MAX);
  sample1 = advanceChannel(112, "run-8", "1", "1");
  sample1 = advanceChannel(114, "run-8", "1", "3");
  assert(sample1["xrm.missingBatches"].as<uint64_t>() == UINT64_MAX);

  // A retained value is a baseline, not evidence that the producer is still
  // running. A later publication proves liveness; unchanged polls do not.
  auto lagConfig = config;
  lagConfig.name = "lag";
  lagConfig.read.key = "lag:CH1";
  lagConfig.sourceHealth.staleAfterMs = 200;
  auto lagFields = waveform(samples16(1, 2));
  lagFields["session"] = "lag-run";
  lagFields["sample_rate_hz"] = "100";
  lagFields["acq_time_valid"] = "1";
  lagFields["acq_time_ns"] = "1";
  assert(publishXrm("lag:CH1", lagFields).ok());
  auto lagRuntime = make(lagConfig);
  lagRuntime->activate();
  assert(lagRuntime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  assert(!lagRuntime->sourceHealth()[0].ready);
  lagFields["batch"] = "2";
  assert(publishXrm("lag:CH1", lagFields).ok());
  eventually([&] { return lagRuntime->sourceHealth()[0].ready; });
  assert(lagRuntime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevNone);
  std::this_thread::sleep_for(250ms);
  assert(!lagRuntime->sourceHealth()[0].ready);
  assert(lagRuntime->sharedPV().fetch()["alarm.severity"].as<int>() == epicsSevInvalid);
  lagConfig.sourceHealth.staleAfterMs = 5000;
  lagRuntime->reconfigure(lagConfig, 2);

  // The stream retains every batch, but the IOC observes only the last hash.
  // Its visible sequence gap counts missed observations, not hardware loss.
  constexpr const char* burst = R"lua(
local id
for i=3,20 do
  local f = {'_',string.char(1,0,2,0),'schema','fermi-ad/xrm-waveform:1',
    'session','lag-run','connection','1','batch',tostring(i),'channel','1',
    'sample_rate_hz','100','sample_count','2','sample_bytes','2',
    'source_validity','valid','acq_time_valid','1','acq_time_ns','1'}
  id = redis.call('XADD',KEYS[1],'*',unpack(f))
  if i == 20 then
    redis.call('DEL',KEYS[2])
    redis.call('HSET',KEYS[2],'contract','fermi-ad/xrm-latest:1',
      'state','ready','commit','lag-run/1/20','stream_id',id,unpack(f))
    redis.call('SET',KEYS[3],'lag-run/1/20')
  end
end
return id
)lua";
  redis.command("EVAL", burst, "3", "{xrm-runtime-tests}:lag:CH1", "{xrm-runtime-tests}:lag:CH1:latest:v1",
                "{xrm-runtime-tests}:lag:latest:v1:commit");
  eventually([&] { return lagRuntime->sharedPV().fetch()["xrm.batch"].as<uint64_t>() == 20; });
  value = lagRuntime->sharedPV().fetch();
  assert(value["xrm.missingBatches"].as<uint64_t>() == 17);
  assert(lagRuntime->sourceHealth()[0].ready);

  // On a transport outage, the retained latest hash is re-baselined and the
  // alarm remains invalid until another publication is observed.
  const auto entriesBeforeOutage = lagRuntime->sourceHealth()[0].reader.entries;
  redis.command("CLIENT", "PAUSE", "1500", "ALL");
  eventually([&] { return !lagRuntime->sourceHealth()[0].reader.connected; });
  assert(!lagRuntime->sourceHealth()[0].ready);
  eventually([&] {
    const auto health = lagRuntime->sourceHealth()[0];
    return health.reader.connected && health.reader.entries > entriesBeforeOutage;
  });
  assert(!lagRuntime->sourceHealth()[0].ready);
  lagFields["batch"] = "21";
  assert(publishXrm("lag:CH1", lagFields).ok());
  eventually([&] { return lagRuntime->sharedPV().fetch()["xrm.batch"].as<uint64_t>() == 21; });
  // The request that observed 21 began after reconnect. Its observation is a
  // deterministic conservative bound for the following publication.
  lagFields["batch"] = "22";
  assert(publishXrm("lag:CH1", lagFields).ok());
  eventually([&] { return lagRuntime->sourceHealth()[0].ready; });
  lagRuntime->deactivate("done");

  // Signed four-byte samples and the exact Unix epoch are representable.
  auto wide = waveform(std::string("\xff\xff\x01\x00\x00\x00\xfe\xff", 8), "4");
  wide["acq_time_valid"] = "1";
  wide["acq_time_ns"] = "0";
  assert(publishXrm("wide", wide).ok());
  auto wideConfig = config;
  wideConfig.name = "wide";
  wideConfig.type = PrimitiveType::Int32;
  wideConfig.read.key = "wide";
  auto wideRuntime = make(wideConfig);
  value = wideRuntime->sharedPV().fetch();
  assert(equals<int32_t>(value, {131071, -131072}));
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 0);
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid); // retained startup baseline
  wideRuntime->activate();
  wide["_"] = std::string("\x01\x00\x00\x00\x02\x00\x00\x00", 8);
  wide["acq_time_ns"] = "18446744073709551615";
  assert(publishXrm("wide", wide).ok());
  eventually([&] { return equals<int32_t>(wideRuntime->sharedPV().fetch(), {1, 2}); });
  value = wideRuntime->sharedPV().fetch();
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 18446744073LL);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 709551615);
  assert(value["alarm.severity"].as<int>() == epicsSevNone);

  auto unsignedBytes = waveform(std::string("\x00\xff", 2), "1");
  unsignedBytes["source_validity"] = "valid";
  unsignedBytes["acq_time_valid"] = "1";
  unsignedBytes["acq_time_ns"] = "1";
  assert(publishXrm("unsigned", unsignedBytes).ok());
  auto unsignedConfig = config;
  unsignedConfig.name = "unsigned";
  unsignedConfig.type = PrimitiveType::UInt8;
  unsignedConfig.read.key = "unsigned";
  auto unsignedRuntime = make(unsignedConfig);
  value = unsignedRuntime->sharedPV().fetch();
  assert(equals<uint8_t>(value, {0, 255}));
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == 1);
  assert(value["alarm.severity"].as<int>() == epicsSevInvalid); // retained startup baseline

  // Legacy Pi streams ignore the new fields and keep deriving time from ID.
  auto legacy = waveform(samples16(60, 61));
  const auto legacyId = producer.addSingleValue<RedisAdapter::Attrs>("legacy", legacy);
  assert(legacyId.ok());
  auto legacyConfig = config;
  legacyConfig.name = "legacy";
  legacyConfig.read.key = "legacy";
  legacyConfig.streamProtocol = StreamProtocol::Legacy;
  auto legacyRuntime = make(legacyConfig);
  value = legacyRuntime->sharedPV().fetch();
  assert(equals<int16_t>(value, {60, 61}));
  assert(!value["xrm"].valid());
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == legacyId.value / 1000000000);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == legacyId.value % 1000000000);
  assert(value["alarm.severity"].as<int>() == epicsSevNone);

  legacyRuntime->deactivate("done");
  unsignedRuntime->deactivate("done");
  wideRuntime->deactivate("done");
  channelRuntime1->deactivate("done");
  channelRuntime2->deactivate("done");
  runtime->deactivate("done");
  std::cout << "XRM timestamps, invalid time, waveform decoding and legacy contract passed\n";
}
