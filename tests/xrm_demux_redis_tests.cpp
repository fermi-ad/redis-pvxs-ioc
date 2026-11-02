#include "xrm_demux.h"
#include "hiredis.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
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
};
void eventually(const std::function<bool()>& test) {
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (!test()) { assert(std::chrono::steady_clock::now() < end); std::this_thread::sleep_for(5ms); }
}
Config fixture(int port, const std::string& name, bool timed = false) {
  return parseConfig("schema_version: 1\nredis: {host: 127.0.0.1, port: " + std::to_string(port) + R"YAML(, command_timeout_ms: 200, poll_interval_ms: 5, retry_ms: 20}
sources:
  - stream: xrm-demux-test:)YAML" + name + R"YAML(:input
    checkpoint: xrm-demux-test:)YAML" + name + R"YAML(:checkpoint
    format: int16-le
    layout: sample-major
    samples_per_channel: 2
    start_id: '0-0'
    retention: 2
    timestamp: )YAML" + (timed ? "{mode: unix-nanoseconds, field: acq_ns, valid_field: acq_valid}" : "{mode: absent}") + R"YAML(
    channels:
      - {channel: 5, raw: xrm-demux-test:)YAML" + name + ":ch5:raw, volts: xrm-demux-test:" + name + R"YAML(:ch5:volts, eslo: 0.5, eoff: 1}
      - {channel: 8, raw: xrm-demux-test:)YAML" + name + ":ch8:raw, volts: xrm-demux-test:" + name + ":ch8:volts, eslo: 2, eoff: -1}\n");
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
