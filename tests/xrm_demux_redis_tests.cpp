#include "xrm_demux.h"
#include "hiredis.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <sstream>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

using namespace xrm_demux;
using namespace std::chrono_literals;
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

struct Redis {
  std::unique_ptr<redisContext, decltype(&redisFree)> context;
  explicit Redis(int port) : context(redisConnect("127.0.0.1", port), &redisFree) {
    assert(context && !context->err);
    const timeval timeout{3, 0}; assert(redisSetTimeout(context.get(), timeout) == REDIS_OK);
  }
  Reply command(const std::vector<std::string>& args) {
    std::vector<const char*> argv; std::vector<size_t> lengths;
    for (const auto& arg : args) { argv.push_back(arg.data()); lengths.push_back(arg.size()); }
    Reply reply(static_cast<redisReply*>(redisCommandArgv(context.get(), static_cast<int>(args.size()), argv.data(), lengths.data())), &freeReplyObject);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
      std::cerr << "fixture Redis command failed: " << (reply ? std::string(reply->str, reply->len) : context->errstr) << '\n';
      std::abort();
    }
    return reply;
  }
  std::string field(const std::string& key, const std::string& name) {
    const auto reply = command({"HGET", key, name});
    return reply->type == REDIS_REPLY_NIL ? "" : std::string(reply->str, reply->len);
  }
  void append(const Record& record, const std::string& id) {
    std::vector<std::string> args{"XADD", record.key, id};
    for (const auto& field : record.fields) { args.push_back(field.first); args.push_back(field.second); }
    command(args);
  }
  std::map<std::string,std::string> row(const std::string& key, const std::string& id) {
    const auto reply = command({"XRANGE",key,id,id,"COUNT","1"});
    assert(reply->type == REDIS_REPLY_ARRAY && reply->elements == 1);
    const auto fields = reply->element[0]->element[1];
    std::map<std::string,std::string> result;
    for (size_t i=0; i<fields->elements; i+=2)
      result.emplace(std::string(fields->element[i]->str,fields->element[i]->len),std::string(fields->element[i+1]->str,fields->element[i+1]->len));
    return result;
  }
  std::string latestId(const std::string& key) {
    const auto reply = command({"XREVRANGE",key,"+","-","COUNT","1"});
    if (!reply->elements) return {};
    const auto id = reply->element[0]->element[0]; return {id->str,id->len};
  }
};
std::string f32(float value) {
  uint32_t bits; std::memcpy(&bits,&value,sizeof bits);
  std::string bytes(4,'\0');
  for (unsigned i=0;i<4;++i) bytes[i] = static_cast<char>((bits>>(8*i)) & 255);
  return bytes;
}
void appendWord(std::string& bytes, uint32_t value, unsigned width) {
  for (unsigned i=0;i<width;++i) bytes.push_back(static_cast<char>((value>>(8*i)) & 255));
}
std::string eventPulse() {
  std::string bytes;
  for (unsigned i=0;i<4;++i) appendWord(bytes,UINT32_C(0xaa55f154),4);
  for (unsigned i=0;i<2;++i) { appendWord(bytes,UINT32_MAX,4); appendWord(bytes,UINT32_C(1234567890),4); }
  for (unsigned sample=0;sample<3;++sample)
    for (unsigned channel=1;channel<=16;++channel) {
      const int count = channel == 1 ? -32517+static_cast<int>(sample) : static_cast<int>((sample+1)*100+channel);
      appendWord(bytes,static_cast<uint16_t>(count),2);
    }
  assert(bytes.size() == 32*(3+1));
  return bytes;
}
void eventuallyAt(unsigned line, const std::function<bool()>& test) {
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!test()) {
    if (std::chrono::steady_clock::now() >= end) { std::cerr << "fixture condition timed out at line " << line << '\n'; std::abort(); }
    std::this_thread::sleep_for(5ms);
  }
}
#define eventually(...) eventuallyAt(__LINE__,__VA_ARGS__)
Config fixture(int port, const std::string& name, bool timed = false, unsigned inputRetention = 0) {
  return parseConfig("schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) + R"YAML(, command_timeout_ms: 200, poll_interval_ms: 5, retry_ms: 20}
sources:
  - stream: xrm-demux-test:)YAML" + name + R"YAML(:input
    checkpoint: xrm-demux-test:)YAML" + name + R"YAML(:checkpoint
    format: int16-le
    layout: sample-major
    samples_per_channel: 2
    start_id: '0-0'
    retention: 2
)YAML" + (inputRetention ? "    input_retention: " + std::to_string(inputRetention) + "\n" : "") +
    "    timestamp: " + (timed ? "{mode: unix-nanoseconds, field: acq_ns, valid_field: acq_valid}" : "{mode: absent}") + R"YAML(
    channels:
      - {channel: 5, raw: xrm-demux-test:)YAML" + name + ":ch5:raw, volts: xrm-demux-test:" + name + R"YAML(:ch5:volts, eslo: 0.5, eoff: 1}
      - {channel: 8, raw: xrm-demux-test:)YAML" + name + ":ch8:raw, volts: xrm-demux-test:" + name + ":ch8:volts, eslo: 2, eoff: -1}\n");
}
std::string currentBlock(const std::string& name, const std::string& gainName, bool timed = false) {
  const std::string prefix = "xrm-current-test:" + name;
  return "  - stream: '" + prefix + ":volts'\n    output: '" + prefix + ":mamps'\n    checkpoint: '" + prefix + R"YAML(:checkpoint'
    samples_per_pulse: 2
    start_id: '5-0'
    retention: 2
    timestamp: )YAML" + (timed ? "{mode: unix-nanoseconds, field: acq_ns, valid_field: acq_valid}" : "{mode: absent}") +
    "\n    gain: {setting: 'xrm-current-test:" + gainName + ":S', applied: 'xrm-current-test:" + gainName + ":R', bootstrap: 2.5}\n";
}
Config currentFixture(int port, const std::string& name, bool timed = false, bool shared = false) {
  auto yaml = "schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) +
    ", command_timeout_ms: 100, poll_interval_ms: 5, retry_ms: 20}\ncurrent_conversions:\n" + currentBlock(name,name,timed);
  if (shared) yaml += currentBlock(name+"-other",name,timed);
  return parseConfig(yaml);
}
Config eventFixture(int port) {
  std::ostringstream yaml;
  yaml << "schema_version: 1\nredis: {host: 127.0.0.1, port: " << port << R"YAML(, command_timeout_ms: 200, poll_interval_ms: 5, retry_ms: 20}
sources:
 - stream: xrm-es-test:input
   checkpoint: xrm-es-test:raw-checkpoint
   format: acq4xx-es-int16-le
   layout: sample-major
   samples_per_channel: 3
   start_id: '0-0'
   retention: 2
   timestamp: {mode: absent}
   channels:
)YAML";
  for (unsigned channel=1;channel<=16;++channel) {
    yaml << "    - {channel: " << channel << ", raw: 'xrm-es-test:CH" << channel
         << ":raw', volts: 'xrm-es-test:CH" << channel << ":volts', eslo: "
         << (channel == 1 ? "0.001, eoff: 0.1" : "0.25, eoff: 0.5") << "}\n";
  }
  yaml << "current_conversions:\n";
  for (unsigned channel=1;channel<=16;++channel) {
    yaml << " - stream: 'xrm-es-test:CH" << channel << ":volts'\n"
         << "   output: 'xrm-es-test:CH" << channel << ":mamps'\n"
         << "   checkpoint: 'xrm-es-test:CH" << channel << ":checkpoint'\n"
         << "   samples_per_pulse: 3\n   start_id: '0-0'\n   retention: 2\n"
         << "   timestamp: {mode: absent}\n"
         << "   gain: {setting: 'xrm-es-test:BCM_M_S', applied: 'xrm-es-test:BCM_M_R', bootstrap: 2.5}\n";
  }
  return parseConfig(yaml.str());
}

// A deliberately slow private loopback peer keeps sending bytes faster than
// an idle socket timeout, but cannot finish its advertised RESP bulk reply.
// This distinguishes an absolute deadline from SO_RCVTIMEO behavior.
struct SlowPeer {
  int listener = -1;
  unsigned port = 0;
  std::atomic<bool> stopping{false};
  std::atomic<unsigned> sent{0};
  std::thread thread;
  SlowPeer() {
    listener = ::socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address) == 0);
    socklen_t length = sizeof address;
    assert(::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&length) == 0);
    port = ntohs(address.sin_port); assert(::listen(listener,1) == 0);
    thread = std::thread([this] {
      const int client = ::accept(listener,nullptr,nullptr);
      if (client < 0) return;
#ifdef SO_NOSIGPIPE
      const int yes = 1; ::setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#endif
      const timeval timeout{1,0}; ::setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
      char request;
      if (::recv(client,&request,1,0) > 0) {
        const char header[] = "$1000000\r\n";
        const auto send = [&](const char* bytes, size_t count) {
#ifdef MSG_NOSIGNAL
          return ::send(client,bytes,count,MSG_NOSIGNAL);
#else
          return ::send(client,bytes,count,0);
#endif
        };
        send(header,sizeof header-1);
        while (!stopping && send("x",1) > 0) { ++sent; std::this_thread::sleep_for(5ms); }
      }
      ::close(client);
    });
  }
  ~SlowPeer() {
    stopping = true;
    ::shutdown(listener,SHUT_RDWR); ::close(listener);
    if (thread.joinable()) thread.join();
  }
};

int main() {
  const auto env = std::getenv("REDIS_PVXS_TEST_REDIS_PORT"); assert(env);
  const int port = std::atoi(env); assert(port > 0);
  Redis redis(port);
  redis.command({"SET","xrm-demux-test:neighbor","untouched"});
  const std::string pulse("\x01\x00\x02\x00\xff\xff\x00\x80",8);

  auto config = fixture(port,"restart"); const auto source = config.sources[0];
  redis.command({"XADD",source.stream,"1-0","_",pulse});
  {
    Worker worker(config); worker.start();
    eventually([&] { return redis.field(source.checkpoint,"last_id") == "1-0"; });
    const auto row = redis.row(source.channels[0].raw,"1-0");
    assert(row.at("_") == std::string("\x01\x00\xff\xff",4));
    assert(row.at("acquisition_time_valid") == "0" && !row.count("acquisition_time_ns"));
    assert(worker.status().processed == 1); worker.stop();
  }
  // Restart consumes only IDs newer than durable checkpoint. A malformed pulse
  // is durably rejected, then subsequent valid pulses continue.
  redis.command({"XADD",source.stream,"2-0","_",pulse});
  redis.command({"XADD",source.stream,"3-0","_",pulse.substr(1)});
  redis.command({"XADD",source.stream,"4-0","_",pulse});
  {
    Worker worker(config); worker.start();
    eventually([&] { return redis.field(source.checkpoint,"last_id") == "4-0"; });
    assert(redis.field(source.checkpoint,"processed") == "3");
    assert(redis.field(source.checkpoint,"rejected") == "1");
    assert(redis.field(source.checkpoint,"last_rejected_id") == "3-0");
    assert(redis.field(source.checkpoint,"last_rejection").find("payload size") != std::string::npos);
    assert(worker.status().processed == 2 && worker.status().rejected == 1);
    assert(redis.command({"XLEN",source.channels[0].raw})->integer == 2);
    assert(redis.command({"XRANGE",source.channels[0].raw,"3-0","3-0"})->elements == 0);
    // Trimmed/deleted cursor cannot prove continuity; its next publication says so.
    redis.command({"XDEL",source.stream,"4-0"});
    redis.command({"XADD",source.stream,"5-0","_",pulse});
    eventually([&] { return redis.field(source.checkpoint,"last_id") == "5-0"; });
    assert(redis.field(source.checkpoint,"gaps") == "1");
    assert(redis.row(source.channels[0].raw,"5-0").at("source_continuity") == "unknown");
    worker.stop();
  }
  // Full timestamps retain exact nanoseconds, backward time, and an invalid
  // producer timestamp; source Redis IDs remain a separate monotonically ordered identity.
  auto times = fixture(port,"timestamps",true); const auto ts = times.sources[0];
  {
    Worker worker(times); worker.start();
    for (const auto& pair : std::vector<std::pair<std::string,std::string>>{{"1-0","9223372036854775807"},{"2-0","1234567890123456789"},{"3-0","-9223372036854775808"}}) {
      const auto valid = pair.first == "3-0" ? "0" : "1";
      redis.command({"XADD",ts.stream,pair.first,"_",pulse,"acq_ns",pair.second,"acq_valid",valid});
      eventually([&] { return redis.field(ts.checkpoint,"last_id") == pair.first; });
      const auto row = redis.row(ts.channels[1].volts,pair.first);
      assert(row.at("acquisition_time_ns") == pair.second && row.at("acquisition_time_valid") == valid);
    }
    worker.stop();
  }
  // A previous partial fanout can be completed only if the existing same-ID
  // record matches every output field and payload.
  auto resume = fixture(port,"partial"); const auto partial = resume.sources[0];
  const auto expected = decode(partial,"10-0",pulse,"","");
  redis.append(expected[0],"10-0");
  redis.command({"XADD",partial.stream,"10-0","_",pulse});
  {
    Worker worker(resume); worker.start();
    eventually([&] { return redis.field(partial.checkpoint,"last_id") == "10-0"; });
    for (const auto& record : expected) assert(redis.row(record.key,"10-0") == record.fields);
    worker.stop();
  }
  // Simulate a crash after the first fanout write, followed by producer trim
  // of BOTH the pending source entry and the previous checkpoint history.
  // The saved pending tuple completes the original fanout deterministically.
  auto retained = fixture(port,"retained-pending"); const auto saved = retained.sources[0];
  const auto savedRecords = decode(saved,"10-0",pulse,"","");
  redis.append(savedRecords[0],"10-0");
  redis.command({"HSET",saved.checkpoint,"config",saved.fingerprint,"source_stream",saved.stream,
    "last_id","0-0","processed","0","rejected","0","gaps","0",
    "pending_id","10-0","pending_kind","entry","pending_payload",pulse,
    "pending_timestamp","","pending_valid","","pending_gap","0","pending_reason",""});
  redis.command({"XADD",saved.stream,"11-0","_",pulse});
  {
    Worker worker(retained); worker.start();
    eventually([&] { return redis.field(saved.checkpoint,"last_id") == "11-0"; });
    for (const auto& record : savedRecords) assert(redis.row(record.key,"10-0") == record.fields);
    assert(redis.field(saved.checkpoint,"processed") == "2");
    assert(redis.field(saved.checkpoint,"gaps") == "1");
    assert(redis.field(saved.checkpoint,"pending_id").empty()); worker.stop();
  }
  auto collision = fixture(port,"collision"); const auto conflicting = collision.sources[0];
  auto conflictingRecord = decode(conflicting,"10-0",pulse,"","")[0]; conflictingRecord.fields["_"] = "wrong";
  redis.append(conflictingRecord,"10-0");
  redis.command({"XADD",conflicting.stream,"10-0","_",pulse});
  {
    Worker worker(collision); worker.start();
    eventually([&] { return worker.status().failures > 0; });
    assert(redis.field(conflicting.checkpoint,"last_id") == "0-0");
    assert(redis.command({"EXISTS",conflicting.channels[0].volts})->integer == 0);
    assert(worker.status().error.find("same-ID output collision") != std::string::npos); worker.stop();
  }
  // Preflight catches even the final output's wrong type before writing the
  // first output. Other configured streams still make progress.
  auto blocked = fixture(port,"wrongtype"); const auto broken = blocked.sources[0];
  auto healthy = fixture(port,"healthy"); const auto good = healthy.sources[0]; blocked.sources.push_back(good);
  redis.command({"SET",broken.channels.back().volts,"wrong-type"});
  redis.command({"XADD",broken.stream,"1-0","_",pulse});
  redis.command({"XADD",good.stream,"1-0","_",pulse});
  {
    Worker worker(blocked); worker.start();
    eventually([&] { return worker.status().failures > 0 && redis.field(good.checkpoint,"last_id") == "1-0"; });
    assert(redis.command({"EXISTS",broken.channels[0].raw})->integer == 0);
    assert(redis.field(broken.checkpoint,"last_id") == "0-0");
    redis.command({"DEL",broken.channels.back().volts}); // fixture repairs its deliberately corrupt key
    eventually([&] { return redis.field(broken.checkpoint,"last_id") == "1-0"; }); worker.stop();
  }
  // Unsupported mixed metadata and oversized input are not returned to the
  // client as payloads; rejection reason and source ID are durable and visible.
  auto invalid = fixture(port,"invalid"); const auto bad = invalid.sources[0];
  redis.command({"XADD",bad.stream,"1-0","_",pulse,"SPAD","1234"});
  redis.command({"XADD",bad.stream,"2-0","_",std::string(2 * 1024 * 1024,'x')});
  {
    Worker worker(invalid); worker.start();
    eventually([&] { return redis.field(bad.checkpoint,"last_id") == "2-0"; });
    assert(redis.field(bad.checkpoint,"rejected") == "2");
    assert(redis.field(bad.checkpoint,"last_error").find("payload size") != std::string::npos);
    assert(worker.status().state == "rejected-input" && worker.status().rejected == 2);
    assert(redis.command({"EXISTS",bad.channels[0].raw})->integer == 0); worker.stop();
  }
  // Reusing a checkpoint under changed interpretation fails closed.
  auto changed = fixture(port,"restart");
  changed = parseConfig("schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) + R"YAML(, retry_ms: 10}
sources:
 - stream: xrm-demux-test:restart:input
   checkpoint: xrm-demux-test:restart:checkpoint
   format: int16-le
   layout: sample-major
   samples_per_channel: 1
   retention: 2
   start_id: '0-0'
   timestamp: {mode: absent}
   channels: [{channel: 1, raw: 'different:raw', volts: 'different:volts', eslo: 1, eoff: 0}]
)YAML");
  {
    Worker worker(changed); worker.start(); eventually([&] { return worker.status().failures > 0; });
    assert(worker.status().error.find("configuration mismatch") != std::string::npos);
    assert(redis.command({"EXISTS","different:raw"})->integer == 0); worker.stop();
  }
  // Two workers racing from one checkpoint still publish each source ID once.
  auto racing = fixture(port,"race"); const auto race = racing.sources[0];
  for (unsigned i=1;i<=20;++i) redis.command({"XADD",race.stream,std::to_string(i)+"-0","_",pulse});
  {
    Worker first(racing), second(racing); first.start(); second.start();
    eventually([&] { return redis.field(race.checkpoint,"last_id") == "20-0"; });
    first.stop(); second.stop();
    assert(redis.field(race.checkpoint,"processed") == "20");
    assert(redis.command({"XLEN",race.channels[0].raw})->integer == 2);
  }
  assert(redis.command({"GET","xrm-demux-test:neighbor"})->len == 9);
  // Conversion can start after an explicit publication boundary while retaining
  // historical output, applies idle gain commands once, and never replays a
  // prior waveform under a new gain or bootstraps over an operator setting.
  const auto currentConfig = currentFixture(port,"restart"); const auto current = currentConfig.currentConversions.front();
  const auto volts = f32(1.0f)+f32(-2.0f);
  redis.command({"XADD",current.stream,"5-0","_",volts});
  redis.command({"XADD",current.output,"5-0","_","legacy-history"});
  std::string firstWave, gainId;
  {
    Worker worker(currentConfig); worker.start();
    eventually([&] { return !redis.latestId(current.applied).empty(); });
    const auto firstAck = redis.latestId(current.applied);
    assert(redis.row(current.applied,firstAck).at("_") == f32(2.5f));
    assert(redis.row(current.output,"5-0").at("_") == "legacy-history");
    redis.command({"XADD",current.stream,"6-0","_",volts});
    eventually([&] { return redis.field(current.checkpoint,"last_id") == "6-0"; });
    auto row = redis.row(current.output,"6-0"); firstWave = row.at("_"); gainId = row.at("gain_command_id");
    assert(firstWave == f32(2.5f)+f32(-5.0f));
    assert(row.at("gain") == f32(2.5f) && row.at("source_id") == "6-0");
    assert(row.at("acquisition_time_valid") == "0" && !row.count("acquisition_time_ns"));
    redis.command({"XADD",current.setting,"*","_",f32(-3.0f)});
    eventually([&] { const auto id=redis.latestId(current.applied); return !id.empty() && redis.row(current.applied,id).at("_") == f32(-3.0f); });
    assert(redis.command({"XLEN",current.applied})->integer == 2);
    assert(redis.latestId(current.applied) > firstAck);
    redis.command({"XADD",current.stream,"7-0","_",volts});
    eventually([&] { return redis.field(current.checkpoint,"last_id") == "7-0"; });
    assert(redis.row(current.output,"6-0").at("_") == firstWave);
    assert(redis.row(current.output,"7-0").at("_") == f32(-3.0f)+f32(6.0f));
    assert(redis.row(current.output,"7-0").at("gain_command_id") != gainId);
    const auto ackCount = redis.command({"XLEN",current.applied})->integer;
    for (const auto& invalidGain : {std::string("bad"),f32(std::numeric_limits<float>::quiet_NaN()),f32(std::numeric_limits<float>::infinity())}) {
      redis.command({"XADD",current.setting,"*","_",invalidGain});
      const auto invalidId = redis.latestId(current.setting);
      eventually([&] { return redis.field(current.checkpoint,"gain_last_rejected_id") == invalidId; });
      assert(!redis.field(current.checkpoint,"gain_last_error").empty());
      assert(redis.command({"XLEN",current.applied})->integer == ackCount);
    }
    redis.command({"XADD",current.setting,"*","_",f32(0.0f)});
    eventually([&] { return redis.field(current.checkpoint,"gain_value") == f32(0.0f); });
    worker.stop();
  }
  const auto ackBeforeRestart = redis.latestId(current.applied);
  {
    Worker worker(currentConfig); worker.start();
    redis.command({"XADD",current.stream,"8-0","_",volts});
    eventually([&] { return redis.field(current.checkpoint,"last_id") == "8-0"; });
    assert(redis.field(current.checkpoint,"gain_value") == f32(0.0f));
    assert(redis.latestId(current.applied) == ackBeforeRestart);
    assert(redis.row(current.output,"8-0").at("_") == f32(0.0f)+f32(-0.0f));
    assert(redis.command({"XLEN",current.output})->integer == 2);
    assert(redis.field(current.checkpoint,"processed") == "3"); worker.stop();
  }
  // One global command updates every configured channel's state before its
  // single readback ack. Existing persisted S takes precedence over bootstrap.
  const auto sharedConfig = currentFixture(port,"shared",false,true);
  const auto sharedCurrent = sharedConfig.currentConversions.front();
  redis.command({"XADD",sharedCurrent.setting,"1-0","_",f32(4.0f)});
  {
    Worker worker(sharedConfig); worker.start();
    eventually([&] { return !redis.latestId(sharedCurrent.applied).empty(); });
    for (const auto& conversion : sharedConfig.currentConversions) {
      assert(redis.field(conversion.checkpoint,"gain_value") == f32(4.0f));
      redis.command({"XADD",conversion.stream,"6-0","_",volts});
    }
    for (const auto& conversion : sharedConfig.currentConversions)
      eventually([&] { return redis.field(conversion.checkpoint,"last_id") == "6-0"; });
    assert(redis.command({"XLEN",sharedCurrent.setting})->integer == 1);
    assert(redis.command({"XLEN",sharedCurrent.applied})->integer == 1);
    worker.stop();
  }
  // A fresh channel inherits the persisted accepted global gain even when no
  // new S command arrives. Hydration neither re-acks history nor changes an
  // older pending waveform's immutable gain snapshot on another new member.
  const auto hydratePrefix = "schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) +
    ", command_timeout_ms: 100, poll_interval_ms: 5, retry_ms: 20}\ncurrent_conversions:\n";
  const auto hydrateBaseYaml = hydratePrefix+currentBlock("hydrate-a","hydrate-group");
  const auto hydrateBaseConfig = parseConfig(hydrateBaseYaml); const auto hydrateA = hydrateBaseConfig.currentConversions.front();
  redis.command({"XADD",hydrateA.setting,"1-0","_",f32(2.0f)});
  redis.command({"XADD",hydrateA.setting,"2-0","_",f32(4.0f)});
  redis.command({"XADD",hydrateA.stream,"6-0","_",volts});
  {
    Worker worker(hydrateBaseConfig); worker.start();
    eventually([&] { return redis.field(hydrateA.checkpoint,"last_id") == "6-0"; }); worker.stop();
  }
  const auto hydrateAck = redis.latestId(hydrateA.applied);
  const auto hydrateOriginal = redis.row(hydrateA.output,"6-0");
  const auto hydrateExpanded = parseConfig(hydrateBaseYaml+currentBlock("hydrate-b","hydrate-group")+currentBlock("hydrate-c","hydrate-group"));
  const auto hydrateB = hydrateExpanded.currentConversions[1], hydrateC = hydrateExpanded.currentConversions[2];
  redis.command({"HSET",hydrateC.checkpoint,"config",hydrateC.fingerprint,"source_stream",hydrateC.stream,
    "last_id","5-0","processed","0","rejected","0","gaps","0","gain_setting",hydrateC.setting,"gain_applied",hydrateC.applied,
    "pending_id","6-0","pending_kind","entry","pending_payload",volts,"pending_timestamp","","pending_valid","","pending_gap","0","pending_reason","",
    "pending_gain",f32(2.0f),"pending_gain_command_id","1-0","pending_metadata","{}"});
  redis.command({"XADD",hydrateA.stream,"7-0","_",volts});
  redis.command({"XADD",hydrateB.stream,"6-0","_",volts});
  redis.command({"XADD",hydrateC.stream,"7-0","_",volts});
  {
    Worker worker(hydrateExpanded); worker.start();
    eventually([&] { return redis.field(hydrateB.checkpoint,"last_id") == "6-0" && redis.field(hydrateC.checkpoint,"last_id") == "7-0"; });
    eventually([&] { return redis.field(hydrateA.checkpoint,"last_id") == "7-0"; });
    assert(redis.row(hydrateB.output,"6-0").at("_") == f32(4.0f)+f32(-8.0f));
    assert(redis.row(hydrateC.output,"6-0").at("_") == f32(2.0f)+f32(-4.0f));
    assert(redis.row(hydrateC.output,"6-0").at("gain_command_id") == "1-0");
    assert(redis.row(hydrateC.output,"7-0").at("_") == f32(4.0f)+f32(-8.0f));
    assert(redis.row(hydrateA.output,"6-0") == hydrateOriginal);
    assert(redis.field(hydrateB.checkpoint,"gain_value") == f32(4.0f));
    assert(redis.field(hydrateC.checkpoint,"gain_command_id") == "2-0");
    assert(redis.latestId(hydrateA.applied) == hydrateAck);
    assert(redis.command({"XLEN",hydrateA.applied})->integer == 1);
    assert(redis.command({"XLEN",hydrateA.setting})->integer == 2); worker.stop();
  }
  // Coordinator migration can replay the latest persisted S, but an exact
  // matching latest R is already its acknowledgement. Reuse that ID while
  // initializing a new lexicographically earlier member. An older in-flight
  // waveform on C must retain its gain through both the command and migration.
  redis.command({"HSET",hydrateC.checkpoint,"pending_id","8-0","pending_kind","entry","pending_payload",volts,
    "pending_timestamp","","pending_valid","","pending_gap","0","pending_reason","",
    "pending_gain",f32(4.0f),"pending_gain_command_id","2-0","pending_metadata","{}"});
  const auto migrationHistory6 = redis.row(hydrateC.output,"6-0"), migrationHistory7 = redis.row(hydrateC.output,"7-0");
  redis.command({"XADD",hydrateC.output,"8-0","_","fixture blocks pending commit"});
  redis.command({"XADD",hydrateA.setting,"*","_",f32(-3.0f)});
  {
    Worker worker(hydrateExpanded); worker.start();
    eventually([&] { return redis.field(hydrateA.checkpoint,"gain_value") == f32(-3.0f) && worker.status().failures > 0; });
    assert(redis.field(hydrateC.checkpoint,"pending_gain") == f32(4.0f)); worker.stop();
  }
  const auto migrationAck = redis.latestId(hydrateA.applied);
  const auto migrationCommand = redis.latestId(hydrateA.setting);
  assert(redis.row(hydrateA.applied,migrationAck).at("_") == f32(-3.0f));
  // XDEL retains last-generated-id, so restore this fixture's saved history
  // into a fresh stream to remove the deliberate collision and its high-water ID.
  redis.command({"DEL",hydrateC.output});
  redis.append({hydrateC.output,migrationHistory6},"6-0");
  redis.append({hydrateC.output,migrationHistory7},"7-0");
  const auto migrationConfig = parseConfig(hydrateBaseYaml+currentBlock("hydrate-b","hydrate-group")+
    currentBlock("hydrate-c","hydrate-group")+currentBlock("hydrate-0","hydrate-group"));
  const auto migrationFresh = migrationConfig.currentConversions.back();
  redis.command({"XADD",migrationFresh.stream,"6-0","_",volts});
  redis.command({"XADD",hydrateC.stream,"9-0","_",volts});
  {
    Worker worker(migrationConfig); worker.start();
    eventually([&] { return redis.field(migrationFresh.checkpoint,"last_id") == "6-0" && redis.field(hydrateC.checkpoint,"last_id") == "9-0"; });
    for (const auto& conversion : migrationConfig.currentConversions) {
      assert(redis.field(conversion.checkpoint,"gain_value") == f32(-3.0f));
      assert(redis.field(conversion.checkpoint,"gain_command_id") == migrationCommand);
    }
    assert(redis.row(migrationFresh.output,"6-0").at("_") == f32(-3.0f)+f32(6.0f));
    assert(redis.row(hydrateC.output,"8-0").at("_") == f32(4.0f)+f32(-8.0f));
    assert(redis.row(hydrateC.output,"8-0").at("gain_command_id") == "2-0");
    assert(redis.row(hydrateC.output,"9-0").at("_") == f32(-3.0f)+f32(6.0f));
    assert(redis.row(hydrateA.output,"6-0") == hydrateOriginal);
    assert(redis.row(hydrateA.stream,"6-0").at("_") == volts);
    assert(redis.command({"XLEN",hydrateA.stream})->integer == 2);
    assert(redis.latestId(hydrateA.applied) == migrationAck);
    assert(redis.command({"XLEN",hydrateA.applied})->integer == 2);
    assert(redis.command({"XLEN",hydrateA.setting})->integer == 3); worker.stop();
  }
  // A rejected latest S must not erase the last accepted gain when a fresh
  // earlier coordinator joins. Its donor includes the rejected command cursor,
  // so the rejected setting is neither replayed nor acknowledged again.
  redis.command({"XADD",hydrateA.setting,"*","_","malformed"});
  const auto rejectedMigrationCommand = redis.latestId(hydrateA.setting);
  {
    Worker worker(migrationConfig); worker.start();
    eventually([&] { return redis.field(migrationFresh.checkpoint,"gain_last_rejected_id") == rejectedMigrationCommand; }); worker.stop();
  }
  const auto rejectedMigrationConfig = parseConfig(hydrateBaseYaml+currentBlock("hydrate-b","hydrate-group")+
    currentBlock("hydrate-c","hydrate-group")+currentBlock("hydrate-0","hydrate-group")+currentBlock("hydrate-00","hydrate-group"));
  const auto rejectedFresh = rejectedMigrationConfig.currentConversions.back();
  redis.command({"XADD",rejectedFresh.stream,"6-0","_",volts});
  {
    Worker worker(rejectedMigrationConfig); worker.start();
    eventually([&] { return redis.field(rejectedFresh.checkpoint,"last_id") == "6-0"; });
    assert(redis.row(rejectedFresh.output,"6-0").at("_") == f32(-3.0f)+f32(6.0f));
    assert(redis.field(rejectedFresh.checkpoint,"gain_command_id") == migrationCommand);
    assert(redis.field(rejectedFresh.checkpoint,"gain_command_cursor") == rejectedMigrationCommand);
    assert(redis.field(rejectedFresh.checkpoint,"gain_last_rejected_id") == rejectedMigrationCommand);
    assert(!redis.field(rejectedFresh.checkpoint,"gain_last_error").empty());
    assert(redis.latestId(hydrateA.applied) == migrationAck);
    assert(redis.command({"XLEN",hydrateA.applied})->integer == 2);
    assert(redis.command({"XLEN",hydrateA.setting})->integer == 4); worker.stop();
  }
  // Persisted S/R can outlive all checkpoints. Reuse an exact matching R only
  // with a canonical legacy-time ID; a Redis-valid sequence of 1,000,000 is
  // outside the legacy nanosecond-remainder bound and must fail closed.
  for (const bool validTime : {true,false}) {
    const auto reuseConfig = currentFixture(port,validTime ? "ack-reuse-valid" : "ack-reuse-invalid");
    const auto reuse = reuseConfig.currentConversions.front();
    const std::string reuseId = validTime ? "100-9" : "100-1000000";
    redis.command({"XADD",reuse.setting,"1-0","_",f32(4.0f)});
    redis.command({"XADD",reuse.applied,reuseId,"_",f32(4.0f),"gain_command_id","1-0"});
    redis.command({"XADD",reuse.stream,"6-0","_",volts});
    Worker worker(reuseConfig); worker.start();
    if (validTime) {
      eventually([&] { return redis.field(reuse.checkpoint,"last_id") == "6-0"; });
      assert(redis.row(reuse.output,"6-0").at("_") == f32(4.0f)+f32(-8.0f));
    } else {
      eventually([&] { return worker.status().failures > 0; });
      assert(worker.status().error.find("invalid legacy time") != std::string::npos);
      assert(redis.field(reuse.checkpoint,"last_id") == "5-0");
      assert(redis.command({"EXISTS",reuse.output})->integer == 0);
    }
    assert(redis.latestId(reuse.applied) == reuseId);
    assert(redis.command({"XLEN",reuse.applied})->integer == 1); worker.stop();
  }
  // A saved pending waveform captures the old gain, even if an idle setting
  // change is applied before replay and the original source was trimmed.
  auto pendingConfig = currentFixture(port,"pending"); const auto pendingCurrent = pendingConfig.currentConversions.front();
  {
    Worker worker(pendingConfig); worker.start();
    eventually([&] { return !redis.latestId(pendingCurrent.applied).empty(); }); worker.stop();
  }
  assert(redis.field(pendingCurrent.checkpoint,"writer_until_ms") == "0");
  const auto oldGainId = redis.field(pendingCurrent.checkpoint,"gain_command_id");
  const auto savedCurrent = convertCurrent(pendingCurrent,"6-0",volts,"","",2.5f,oldGainId);
  redis.append(savedCurrent,"6-0");
  redis.command({"HSET",pendingCurrent.checkpoint,"pending_id","6-0","pending_kind","entry","pending_payload",volts,
    "pending_timestamp","","pending_valid","","pending_gap","0","pending_reason","","pending_gain",f32(2.5f),
    "pending_gain_command_id",oldGainId,"pending_metadata","{}"});
  redis.command({"XADD",pendingCurrent.setting,"*","_",f32(5.0f)});
  redis.command({"XADD",pendingCurrent.stream,"7-0","_",volts});
  {
    Worker worker(pendingConfig); worker.start();
    eventually([&] { return redis.field(pendingCurrent.checkpoint,"last_id") == "7-0"; });
    assert(redis.row(pendingCurrent.output,"6-0") == savedCurrent.fields);
    assert(redis.row(pendingCurrent.output,"7-0").at("_") == f32(5.0f)+f32(-10.0f));
    assert(redis.field(pendingCurrent.checkpoint,"processed") == "2"); worker.stop();
  }
  // Timestamp handling is identical to raw demux: exact, possibly backward,
  // invalid or absent acquisition nanoseconds, with no host substitution.
  const auto timedCurrentConfig = currentFixture(port,"times",true); const auto timedCurrent = timedCurrentConfig.currentConversions.front();
  {
    Worker worker(timedCurrentConfig); worker.start();
    redis.command({"XADD",timedCurrent.stream,"6-0","_",volts,"acq_ns","9223372036854775807","acq_valid","1"});
    redis.command({"XADD",timedCurrent.stream,"7-0","_",volts,"acq_ns","-9223372036854775808","acq_valid","0"});
    eventually([&] { return redis.field(timedCurrent.checkpoint,"last_id") == "7-0"; });
    assert(redis.row(timedCurrent.output,"6-0").at("acquisition_time_ns") == "9223372036854775807");
    assert(redis.row(timedCurrent.output,"7-0").at("acquisition_time_valid") == "0");
    redis.command({"XADD",timedCurrent.stream,"8-0","_",volts,"acq_valid","1"});
    redis.command({"XADD",timedCurrent.stream,"9-0","_",volts.substr(1)});
    redis.command({"XADD",timedCurrent.stream,"10-0","_",volts});
    eventually([&] { return redis.field(timedCurrent.checkpoint,"last_id") == "10-0"; });
    assert(redis.field(timedCurrent.checkpoint,"rejected") == "2");
    assert(!redis.row(timedCurrent.output,"10-0").count("acquisition_time_ns")); worker.stop();
  }
  const auto leasedConfig = currentFixture(port,"lease"); const auto leased = leasedConfig.currentConversions.front();
  {
    Worker first(leasedConfig), second(leasedConfig); first.start();
    eventually([&] { return !redis.latestId(leased.applied).empty(); });
    second.start(); eventually([&] { return second.status().failures > 0; });
    assert(second.status().error.find("writer lease") != std::string::npos);
    first.stop();
    redis.command({"XADD",leased.stream,"6-0","_",volts});
    eventually([&] { return redis.field(leased.checkpoint,"last_id") == "6-0"; });
    assert(redis.field(leased.checkpoint,"processed") == "1"); second.stop();
  }
  // A current lane blocked in long gain-lease retry must not hold raw input
  // behind it. Existing bounded VOLTS streams carry the handoff; degraded
  // status remains visible while the independent raw lane makes progress.
  const auto lanesConfig = parseConfig("schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) + R"YAML(, command_timeout_ms: 100, poll_interval_ms: 5, retry_ms: 30000}
sources:
 - stream: xrm-lanes-test:input
   checkpoint: xrm-lanes-test:raw-checkpoint
   format: int16-le
   layout: sample-major
   samples_per_channel: 2
   start_id: '0-0'
   retention: 10
   timestamp: {mode: absent}
   channels:
    - {channel: 1, raw: xrm-lanes-test:counts, volts: xrm-lanes-test:volts, eslo: 0.5, eoff: 1}
current_conversions:
 - stream: xrm-lanes-test:volts
   output: xrm-lanes-test:mamps
   checkpoint: xrm-lanes-test:current-checkpoint
   samples_per_pulse: 2
   start_id: '0-0'
   retention: 10
   timestamp: {mode: absent}
   gain: {setting: xrm-lanes-test:S, applied: xrm-lanes-test:R, bootstrap: 2.5}
)YAML");
  const auto laneRaw = lanesConfig.sources.front();
  const auto laneCurrent = lanesConfig.currentConversions.front();
  const auto laneTime = redis.command({"TIME"});
  const auto laneServerMs = std::stoull(std::string(laneTime->element[0]->str,laneTime->element[0]->len))*1000 +
    std::stoull(std::string(laneTime->element[1]->str,laneTime->element[1]->len))/1000;
  redis.command({"HSET",laneCurrent.checkpoint,"config",laneCurrent.fingerprint,"source_stream",laneCurrent.stream,
    "last_id","0-0","processed","0","rejected","0","gaps","0","gain_setting",laneCurrent.setting,"gain_applied",laneCurrent.applied,
    "writer_token","fixture-held","writer_until_ms",std::to_string(laneServerMs+60000)});
  const std::string lanePulse("\x01\x00\xff\xff",4);
  {
    Worker worker(lanesConfig); worker.start();
    eventually([&] { return worker.status().failures > 0; });
    for (unsigned id=1;id<=5;++id) redis.command({"XADD",laneRaw.stream,std::to_string(id)+"-0","_",lanePulse});
    eventually([&] { return redis.field(laneRaw.checkpoint,"last_id") == "5-0"; });
    assert(redis.field(laneCurrent.checkpoint,"last_id") == "0-0");
    auto status = worker.status();
    assert(status.running && status.processed == 5 && status.state == "retrying");
    assert(status.error.find("writer lease") != std::string::npos);
    bool duplicateStart = false;
    try { worker.start(); } catch (const std::exception&) { duplicateStart = true; }
    assert(duplicateStart);
    redis.command({"XADD",laneRaw.stream,"6-0","_",lanePulse});
    eventually([&] { return redis.field(laneRaw.checkpoint,"last_id") == "6-0"; });
    const auto stopped = std::chrono::steady_clock::now(); worker.stop();
    assert(std::chrono::steady_clock::now()-stopped < 1s);
    assert(!worker.status().running && worker.status().state == "stopped");
    assert(redis.field(laneCurrent.checkpoint,"writer_token") == "fixture-held");
  }
  redis.command({"HSET",laneCurrent.checkpoint,"writer_until_ms","0"});
  {
    Worker worker(lanesConfig); worker.start();
    redis.command({"XADD",laneRaw.stream,"7-0","_",lanePulse});
    eventually([&] { return redis.field(laneCurrent.checkpoint,"last_id") == "7-0"; });
    assert(redis.row(laneCurrent.output,"7-0").at("_") == f32(3.75f)+f32(1.25f));
    assert(redis.field(laneRaw.checkpoint,"processed") == "7");
    assert(redis.field(laneCurrent.checkpoint,"processed") == "7");
    worker.stop();
    assert(!worker.status().running);
    assert(redis.field(laneCurrent.checkpoint,"writer_until_ms") == "0");
  }
  // Sixteen selected channels share one canonical command/ack coordinator.
  // One MiB of source data exercises the observed 16,384-sample profile at
  // low fixture cost and verifies all worker states before readback appears.
  auto allYaml = "schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) +
    ", command_timeout_ms: 200, poll_interval_ms: 5, retry_ms: 20}\ncurrent_conversions:\n";
  for (unsigned i=1;i<=16;++i) {
    auto block = currentBlock("all-"+std::to_string(i),"all");
    const auto position = block.find("samples_per_pulse: 2"); assert(position != std::string::npos);
    block.replace(position,std::string("samples_per_pulse: 2").size(),"samples_per_pulse: 16384");
    allYaml += block;
  }
  const auto allConfig = parseConfig(allYaml); const auto allCurrent = allConfig.currentConversions.front();
  std::string fullVolts, fullCurrent;
  for (unsigned i=0;i<16384;++i) { fullVolts += f32(1.0f); fullCurrent += f32(2.5f); }
  {
    Worker worker(allConfig); worker.start();
    eventually([&] { return !redis.latestId(allCurrent.applied).empty(); });
    for (const auto& conversion : allConfig.currentConversions) {
      assert(redis.field(conversion.checkpoint,"gain_value") == f32(2.5f));
      redis.command({"XADD",conversion.stream,"6-0","_",fullVolts});
    }
    for (const auto& conversion : allConfig.currentConversions) {
      eventually([&] { return redis.field(conversion.checkpoint,"last_id") == "6-0"; });
      assert(redis.row(conversion.output,"6-0").at("_") == fullCurrent);
    }
    assert(redis.command({"XLEN",allCurrent.applied})->integer == 1);
    redis.command({"XADD",allCurrent.setting,"*","_",f32(-1.5f)});
    eventually([&] { const auto id=redis.latestId(allCurrent.applied); return redis.row(allCurrent.applied,id).at("_") == f32(-1.5f); });
    for (const auto& conversion : allConfig.currentConversions)
      assert(redis.field(conversion.checkpoint,"gain_value") == f32(-1.5f));
    assert(redis.command({"XLEN",allCurrent.applied})->integer == 2); worker.stop();
  }
  // Opposite YAML orders must acquire the same canonical first checkpoint.
  // Reserve that checkpoint to expose partial acquisition deterministically:
  // the blocked reverse-order worker must not retain leases on a prefix of
  // the other channels. After release, one worker progresses, then hands over.
  auto reversedConfig = allConfig;
  std::reverse(reversedConfig.currentConversions.begin(),reversedConfig.currentConversions.end());
  const auto canonical = std::min_element(allConfig.currentConversions.begin(),allConfig.currentConversions.end(),
    [](const CurrentConversion& a, const CurrentConversion& b) { return a.checkpoint < b.checkpoint; })->checkpoint;
  for (const auto& conversion : allConfig.currentConversions)
    redis.command({"HSET",conversion.checkpoint,"writer_token","","writer_until_ms","0"});
  const auto serverTime = redis.command({"TIME"});
  const auto serverMs = std::stoull(std::string(serverTime->element[0]->str,serverTime->element[0]->len))*1000 +
    std::stoull(std::string(serverTime->element[1]->str,serverTime->element[1]->len))/1000;
  redis.command({"HSET",canonical,"writer_token","fixture-reserved","writer_until_ms",std::to_string(serverMs+30000)});
  const auto contentionAck = redis.latestId(allCurrent.applied);
  for (const auto& conversion : allConfig.currentConversions) redis.command({"XADD",conversion.stream,"7-0","_",fullVolts});
  {
    Worker reverse(reversedConfig), forward(allConfig); reverse.start();
    eventually([&] { return reverse.status().failures > 0; });
    for (const auto& conversion : allConfig.currentConversions)
      if (conversion.checkpoint != canonical) assert(redis.field(conversion.checkpoint,"writer_until_ms") == "0");
    forward.start(); eventually([&] { return forward.status().failures > 0; });
    redis.command({"HSET",canonical,"writer_token","","writer_until_ms","0"});
    for (const auto& conversion : allConfig.currentConversions)
      eventually([&] { return redis.field(conversion.checkpoint,"last_id") == "7-0"; });
    const bool reverseWon = reverse.status().processed > 0;
    if (reverseWon) reverse.stop(); else forward.stop();
    for (const auto& conversion : allConfig.currentConversions) redis.command({"XADD",conversion.stream,"8-0","_",fullVolts});
    for (const auto& conversion : allConfig.currentConversions) {
      eventually([&] { return redis.field(conversion.checkpoint,"last_id") == "8-0"; });
      assert(redis.field(conversion.checkpoint,"processed") == "3");
    }
    if (reverseWon) { assert(forward.status().processed > 0); forward.stop(); }
    else { assert(reverse.status().processed > 0); reverse.stop(); }
    assert(redis.latestId(allCurrent.applied) == contentionAck);
    assert(redis.command({"XLEN",allCurrent.applied})->integer == 2);
  }
  // A raw output is the one intentional key-role overlap. Current accepts
  // only bounded known demux metadata and retains original source provenance.
  auto chainedYaml = "schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) + R"YAML(, poll_interval_ms: 5, retry_ms: 20}
sources:
 - stream: xrm-current-test:chain:raw-input
   checkpoint: xrm-current-test:chain:raw-checkpoint
   format: int16-le
   layout: sample-major
   samples_per_channel: 2
   start_id: '0-0'
   retention: 2
   timestamp: {mode: absent}
   channels:
    - {channel: 1, raw: 'xrm-current-test:chain:counts', volts: 'xrm-current-test:chain:volts', eslo: 0.5, eoff: 1}
current_conversions:
)YAML" + currentBlock("chain","chain");
  const auto boundary = chainedYaml.find("start_id: '5-0'"); assert(boundary != std::string::npos);
  chainedYaml.replace(boundary,std::string("start_id: '5-0'").size(),"start_id: '0-0'");
  const auto chainedConfig = parseConfig(chainedYaml); const auto chained = chainedConfig.currentConversions.front();
  redis.command({"XADD",chainedConfig.sources.front().stream,"6-0","_",std::string("\x02\x00\xfe\xff",4)});
  {
    Worker worker(chainedConfig); worker.start();
    eventually([&] { return redis.field(chained.checkpoint,"last_id") == "6-0"; });
    const auto row = redis.row(chained.output,"6-0");
    assert(row.at("_") == f32(5.0f)+f32(0.0f));
    assert(row.at("upstream_source_stream") == chainedConfig.sources.front().stream);
    assert(row.at("upstream_source_id") == "6-0" && row.at("channel") == "1");
    worker.stop();
  }
  // The 16-channel ES profile strips exactly one 32-byte event frame before
  // demux and current conversion. Three ADC frames remain three samples on
  // every channel; neither magic words nor counters become voltage samples.
  const auto esConfig = eventFixture(port); const auto es = esConfig.sources.front();
  const auto esPulse = eventPulse();
  auto badMagic = esPulse; badMagic[0] ^= 1;
  auto badCounters = esPulse; badCounters[24] ^= 1;
  {
    Worker worker(esConfig); worker.start();
    eventually([&] { return !redis.latestId(esConfig.currentConversions.front().applied).empty(); });
    unsigned rejected = 0;
    for (const auto& badPulse : {esPulse+std::string(32,'\0'),badMagic,badCounters}) {
      const auto id = std::to_string(++rejected)+"-0";
      redis.command({"XADD",es.stream,id,"_",badPulse});
      eventually([&] { return redis.field(es.checkpoint,"last_id") == id; });
      assert(redis.field(es.checkpoint,"rejected") == std::to_string(rejected));
      assert(redis.field(es.checkpoint,"last_rejected_id") == id);
      assert(!redis.field(es.checkpoint,"last_rejection").empty());
      assert(redis.command({"EXISTS",es.channels.front().volts})->integer == 0);
    }
    assert(redis.field(es.checkpoint,"last_rejection").find("counter pairs") != std::string::npos);
    redis.command({"XADD",es.stream,"4-0","_",esPulse});
    for (const auto& conversion : esConfig.currentConversions)
      eventually([&] { return redis.field(conversion.checkpoint,"last_id") == "4-0"; });
    assert(redis.field(es.checkpoint,"processed") == "1");
    assert(redis.field(es.checkpoint,"last_rejected_id") == "3-0");
    assert(redis.command({"XLEN",es.stream})->integer == 4); // input_retention is absent
    assert(redis.row(es.stream,"4-0").at("_") == esPulse);
    assert(redis.command({"XLEN",esConfig.currentConversions.front().applied})->integer == 1);
    // NumPy 2.4 float32 fixtures, independently evaluated as
    // int16[-32517,-32516,-32515] * float32(.001) + float32(.1),
    // followed by float32(2.5). A double intermediate differs in the low bit.
    const std::string referenceVolts("\x03\xab\x01\xc2\xfd\xa9\x01\xc2\xf7\xa8\x01\xc2",12);
    const std::string referenceMamps("\xc4\x15\xa2\xc2\x7c\x14\xa2\xc2\x35\x13\xa2\xc2",12);
    for (unsigned i=0;i<16;++i) {
      const auto& channel = es.channels[i]; const auto& conversion = esConfig.currentConversions[i];
      const auto raw = redis.row(channel.raw,"4-0"), calibrated = redis.row(channel.volts,"4-0"), currentRow = redis.row(conversion.output,"4-0");
      std::string expectedRaw, expectedVolts, expectedCurrent;
      for (unsigned sample=0;sample<3;++sample) {
        const int count = i == 0 ? -32517+static_cast<int>(sample) : static_cast<int>((sample+1)*100+i+1);
        appendWord(expectedRaw,static_cast<uint16_t>(count),2);
        if (i != 0) {
          // These quarter-step values and their 2.5 products are exactly binary32.
          const float v = float(count)/4.0f+0.5f;
          expectedVolts += f32(v); expectedCurrent += f32(v*2.5f);
        }
      }
      assert(raw.at("_") == expectedRaw && raw.at("_").size() == 6);
      assert(calibrated.at("_") == (i == 0 ? referenceVolts : expectedVolts));
      assert(currentRow.at("_") == (i == 0 ? referenceMamps : expectedCurrent));
      for (const auto& fields : {raw,calibrated,currentRow}) {
        assert(fields.at("event_counter_0") == "4294967295");
        assert(fields.at("event_counter_1") == "1234567890");
        assert(fields.at("acquisition_time_valid") == "0" && !fields.count("acquisition_time_ns"));
        assert(fields.at("source_id") == "4-0");
      }
      assert(currentRow.at("upstream_source_stream") == es.stream);
      assert(currentRow.at("upstream_source_id") == "4-0");
      assert(currentRow.at("source_stream") == channel.volts);
      assert(currentRow.at("channel") == std::to_string(i+1));
    }
    worker.stop();
  }
  // Explicit input ownership permits exact-key MAXLEN trimming only after a
  // committed result. A six-entry backlog exceeds the bound: after ID1 commits,
  // IDs2..4 are removed, ID5 records the continuity gap, then ID6 completes.
  // Neighbouring history and gain setting streams are outside that grant.
  const auto ownedConfig = fixture(port,"owned-retention",false,2); const auto owned = ownedConfig.sources.front();
  const std::vector<std::string> neighbors{"xrm-demux-test:owned-neighbor","xrm-demux-test:unrelated-BCM_M_S","xrm-demux-test:unrelated-BCM_M_R"};
  for (const auto& key : neighbors)
    for (unsigned id=1;id<=4;++id) redis.command({"XADD",key,std::to_string(id)+"-0","_",f32(2.5f)});
  for (unsigned id=1;id<=6;++id) redis.command({"XADD",owned.stream,std::to_string(id)+"-0","_",pulse});
  redis.command({"SET",owned.channels.back().volts,"fixture blocks commit"});
  {
    Worker worker(ownedConfig); worker.start();
    eventually([&] { return worker.status().failures > 0; });
    assert(redis.field(owned.checkpoint,"last_id") == "0-0");
    assert(redis.field(owned.checkpoint,"pending_id") == "1-0");
    assert(redis.command({"XLEN",owned.stream})->integer == 6); // a failed commit cannot trim input
    redis.command({"DEL",owned.channels.back().volts}); // repair only this fixture's corrupt output
    eventually([&] { return redis.field(owned.checkpoint,"last_id") == "6-0"; });
    assert(redis.command({"XLEN",owned.stream})->integer == 2);
    assert(redis.field(owned.checkpoint,"processed") == "3");
    assert(redis.field(owned.checkpoint,"gaps") == "1");
    assert(redis.field(owned.checkpoint,"last_gap_id") == "5-0");
    assert(redis.row(owned.channels.front().raw,"5-0").at("source_continuity") == "unknown");
    assert(redis.row(owned.channels.front().raw,"6-0").at("source_continuity") == "observed");
    assert(redis.command({"XRANGE",owned.stream,"2-0","4-0","COUNT","1"})->elements == 0);
    for (const auto& key : neighbors) {
      assert(redis.command({"XLEN",key})->integer == 4);
      assert(redis.row(key,"1-0").at("_") == f32(2.5f));
      assert(redis.row(key,"4-0").at("_") == f32(2.5f));
    }
    worker.stop();
  }
  // Same-ID output collisions remain fatal to that source without advancing
  // its cursor; an uncertain timeout reconnects and recovers from Redis state.
  const auto ccConfig = currentFixture(port,"collision"); const auto cc = ccConfig.currentConversions.front();
  redis.command({"XADD",cc.output,"6-0","_","wrong"});
  redis.command({"XADD",cc.stream,"6-0","_",volts});
  {
    Worker worker(ccConfig); worker.start(); eventually([&] { return worker.status().failures > 0; });
    assert(redis.field(cc.checkpoint,"last_id") == "5-0");
    assert(worker.status().error.find("same-ID output collision") != std::string::npos); worker.stop();
  }
  const auto recoveryConfig = currentFixture(port,"recovery"); const auto recovery = recoveryConfig.currentConversions.front();
  {
    Worker worker(recoveryConfig); worker.start();
    eventually([&] { return !redis.latestId(recovery.applied).empty(); });
    redis.command({"CLIENT","PAUSE","400","ALL"});
    eventually([&] { return worker.status().failures > 0; });
    redis.command({"XADD",recovery.stream,"6-0","_",volts});
    eventually([&] { return redis.field(recovery.checkpoint,"last_id") == "6-0"; }); worker.stop();
  }
  assert(redis.command({"GET","xrm-demux-test:neighbor"})->len == 9);
  // An unavailable Redis cannot hold stop() through a retry backoff.
  auto unavailable = fixture(1,"unavailable"); unavailable.retryMs = 30000;
  Worker unavailableWorker(unavailable); unavailableWorker.start();
  eventually([&] { return unavailableWorker.status().failures > 0; });
  const auto before = std::chrono::steady_clock::now(); unavailableWorker.stop();
  assert(std::chrono::steady_clock::now() - before < 1s);
  {
    SlowPeer peer;
    auto slow = fixture(peer.port,"slow-reply"); slow.commandTimeoutMs = 100; slow.retryMs = 30000;
    Worker worker(slow); const auto started = std::chrono::steady_clock::now(); worker.start();
    eventually([&] { return worker.status().failures > 0; });
    assert(std::chrono::steady_clock::now() - started < 1s);
    assert(peer.sent > 3 && worker.status().error.find("deadline") != std::string::npos); worker.stop();
  }
  {
    SlowPeer peer;
    auto slow = fixture(peer.port,"stop-during-reply"); slow.commandTimeoutMs = 2000;
    Worker worker(slow); worker.start(); eventually([&] { return peer.sent > 3; });
    const auto started = std::chrono::steady_clock::now(); worker.stop();
    assert(std::chrono::steady_clock::now() - started < 500ms);
  }
  std::cout << "xrm demux isolated Redis fixture tests passed\n";
}
