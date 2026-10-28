#include "redis_pvxs_ioc/xrm_hash_reader.h"
#include "redis_pvxs_ioc/xrm_contract.h"

#include <sw/redis++/redis++.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
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

std::string str(const redisReply* reply) {
  assert(reply && reply->type == REDIS_REPLY_STRING);
  return {reply->str, reply->len};
}

uint64_t commandCount(sw::redis::Redis& redis, const std::string& command) {
  const auto reply = redis.command("INFO", "commandstats");
  const auto info = str(reply.get());
  const auto needle = "cmdstat_" + command + ":calls=";
  const auto pos = info.find(needle);
  return pos == std::string::npos ? 0 : std::stoull(info.substr(pos + needle.size()));
}

// Local-only forwarding fixture: delay server responses without changing the
// Redis server or the time at which it executes each command.
class DelayedProxy {
public:
  explicit DelayedProxy(uint16_t upstreamPort) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(listener_ >= 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    assert(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t n = sizeof(address);
    assert(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &n) == 0);
    port_ = ntohs(address.sin_port);
    assert(::listen(listener_, 1) == 0);
    worker_ = std::thread([this, upstreamPort] {
      const int downstream = ::accept(listener_, nullptr, nullptr);
      if (downstream < 0) return;
      const int upstream = ::socket(AF_INET, SOCK_STREAM, 0);
      assert(upstream >= 0);
#ifdef SO_NOSIGPIPE
      int enabled = 1;
      assert(::setsockopt(downstream, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
      assert(::setsockopt(upstream, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0);
#endif
      sockaddr_in target{}; target.sin_family = AF_INET;
      target.sin_addr.s_addr = htonl(INADDR_LOOPBACK); target.sin_port = htons(upstreamPort);
      assert(::connect(upstream, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0);
      pollfd fds[2]{{downstream, POLLIN, 0}, {upstream, POLLIN, 0}};
      while (!stopped_) {
        const int rc = ::poll(fds, 2, 20);
        if (rc < 0) break;
        for (int i = 0; i < 2; ++i) {
          if (!(fds[i].revents & POLLIN)) continue;
          char bytes[65536];
          const auto count = ::recv(fds[i].fd, bytes, sizeof(bytes), 0);
          if (count <= 0) { stopped_ = true; break; }
          if (i == 1) std::this_thread::sleep_for(300ms);
          size_t offset = 0;
          while (offset < static_cast<size_t>(count)) {
#ifdef MSG_NOSIGNAL
            const auto sent = ::send(fds[1-i].fd, bytes + offset, count - offset, MSG_NOSIGNAL);
#else
            const auto sent = ::send(fds[1-i].fd, bytes + offset, count - offset, 0);
#endif
            if (sent <= 0) { stopped_ = true; break; }
            offset += static_cast<size_t>(sent);
          }
        }
        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) break;
      }
      ::close(upstream); ::close(downstream);
    });
  }
  ~DelayedProxy() {
    stopped_ = true;
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (worker_.joinable()) worker_.join();
  }
  uint16_t port() const { return port_; }
private:
  int listener_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> stopped_{false};
  std::thread worker_;
};

int main() {
  const auto* port = std::getenv("REDIS_PVXS_TEST_REDIS_PORT");
  assert(port);
  RedisConfig config;
  config.baseKey = "xrm-hash-test";
  config.port = static_cast<uint16_t>(std::stoi(port));
  sw::redis::ConnectionOptions options;
  options.host = config.host; options.port = config.port;
  sw::redis::Redis redis(options);
  constexpr const char* fixturePublish = R"lua(
local fields = {}
for i=2,#ARGV do fields[#fields+1] = ARGV[i] end
local id = redis.call('XADD', KEYS[1], '*', unpack(fields))
redis.call('DEL', KEYS[2])
redis.call('HSET', KEYS[2], 'contract', 'fermi-ad/xrm-latest:1', 'state', 'ready', 'commit', ARGV[1], 'stream_id', id, unpack(fields))
redis.call('SET', KEYS[3], ARGV[1])
return id
)lua";
  const auto stream = "{" + config.baseKey + "}:waveform";
  const auto hash = stream + ":latest:v1";
  const auto publishTo = [&](const std::string& subkey, uint64_t batch) {
    const auto destination = "{" + config.baseKey + "}:" + subkey;
    std::vector<std::string> args{"EVAL", fixturePublish, "3", destination, xrmLatestHashKey(destination),
      xrmCommitMarkerKey(destination), "session1/1/" + std::to_string(batch),
      "_", "abcdefgh", "schema", "fermi-ad/xrm-waveform:1", "session", "session1",
      "connection", "1", "batch", std::to_string(batch), "channel", "1",
      "sample_count", "4", "sample_bytes", "2", "sample_rate_hz", "1000",
      "source_validity", "valid", "acq_time_valid", "1", "acq_time_ns", "1"};
    return str(redis.command(args.begin(), args.end()).get());
  };
  const auto publish = [&](uint64_t batch) { return publishTo("waveform", batch); };

  publish(1); publish(2); const auto third = publish(3);
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<uint64_t> received;
  bool hold = false, entered = false, release = false;
  XrmHashReader reader(config, "waveform", 1024, 5,
      [&](const auto&, const auto&, const auto& entries, const auto&) {
        std::unique_lock<std::mutex> lock(mutex);
        if (entries[0].second.empty()) { received.push_back(0); return; }
        received.push_back(std::stoull(entries[0].second.at("batch")));
        if (hold) {
          entered = true; changed.notify_all();
          changed.wait(lock, [&] { return release; });
          hold = false;
        }
      });
  const auto snapshot = reader.snapshot();
  assert(snapshot.present() && snapshot.id == third && snapshot.fields.at("batch") == "3");
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  reader.start();
  publish(4);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 4; });
  const auto proof = reader.freshnessProof();
  assert(proof != std::chrono::steady_clock::time_point{});
  std::this_thread::sleep_for(100ms);
  assert(reader.freshnessProof() == proof); // polling an unchanged value is not producer liveness

  // Even a huge arbitrary Stream tail is never read by the hash consumer.
  redis.command("XADD", stream, "*", "_", std::string(8 * 1024 * 1024, 'x'));
  std::this_thread::sleep_for(50ms);
  assert(reader.status().cursor != "0-0" && reader.freshnessProof() == proof);

  // An unrecognized giant field is not enumerated or copied by the reader.
  redis.command("HSET", hash, "unrecognized", std::string(8 * 1024 * 1024, 'u'));
  std::this_thread::sleep_for(50ms);
  assert(reader.status().readRejections == 0 && reader.freshnessProof() == proof);
  redis.command("HDEL", hash, "unrecognized");
  const std::string unknownName(8 * 1024 * 1024, 'n');
  redis.command("HSET", hash, unknownName, "x");
  std::this_thread::sleep_for(50ms);
  assert(reader.status().readRejections == 0 && reader.freshnessProof() == proof);
  redis.command("HDEL", hash, unknownName);

  // An oversized known hash field is rejected without an HGET of any field.
  redis.command("HSET", hash, "_", std::string(8 * 1024 * 1024, 'x'));
  eventually([&] { return reader.status().readRejections == 1; });
  const auto gets = commandCount(redis, "hget");
  std::this_thread::sleep_for(80ms);
  assert(commandCount(redis, "hget") == gets);
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(5); // rejected data forces a new baseline
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 5; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(6);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 6; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});

  redis.command("HSET", hash, "stream_id", "not-an-id");
  eventually([&] { return reader.status().readRejections == 2; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(7);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 7; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(8);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 8; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});

  const auto oldEpoch = reader.status().epoch;
  redis.command("DEL", hash);
  eventually([&] { return reader.status().streamKind == RedisAdapter::StreamKind::Missing; });
  assert(reader.status().epoch == oldEpoch + 1 && reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(9); // a publication after observed absence is fresh evidence
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 9; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});
  redis.command("HSET", hash, "stream_id", "1-0");
  eventually([&] { return reader.status().epoch == oldEpoch + 2; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(10);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 10; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});
  redis.command("EVAL", "redis.call('HSET',KEYS[1],'batch','11','commit','session1/1/11'); "
                       "redis.call('SET',KEYS[2],'session1/1/11'); return 1", "2", hash,
                xrmCommitMarkerKey(stream));
  eventually([&] { return reader.status().epoch == oldEpoch + 3; });
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 11; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(12);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 12; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});

  // Redis outage clears freshness even if the same retained hash is available
  // after reconnect; it takes another publication to re-establish proof.
  redis.command("CLIENT", "PAUSE", "1500", "ALL");
  eventually([&] { return !reader.status().connected && reader.status().readFailures > 0; });
  eventually([&] { return reader.status().connected && reader.status().streamKind == RedisAdapter::StreamKind::Stream; });
  assert(reader.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(13);
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 13; });
  assert(reader.freshnessProof() != std::chrono::steady_clock::time_point{});

  // If the callback cannot keep up, the hash coalesces intermediate updates.
  { std::lock_guard<std::mutex> guard(mutex); hold = true; }
  publish(14);
  {
    std::unique_lock<std::mutex> lock(mutex);
    assert(changed.wait_for(lock, 5s, [&] { return entered; }));
  }
  for (int i = 15; i <= 114; ++i) publish(i);
  { std::lock_guard<std::mutex> guard(mutex); release = true; }
  changed.notify_all();
  eventually([&] { std::lock_guard<std::mutex> guard(mutex); return !received.empty() && received.back() == 114; });
  {
    std::lock_guard<std::mutex> guard(mutex);
    for (const auto batch : received) assert(batch <= 14 || batch == 114);
  }
  reader.reset();

  // An oversized compact hash must be rejected before field lookup, including
  // when an unusual server configuration permits very large listpacks.
  redis.command("CONFIG", "SET", "hash-max-listpack-value", "1048576");
  const auto compact = "{" + config.baseKey + "}:compact:latest:v1";
  redis.command("HSET", compact, "unrecognized", std::string(131072, 'x'));
  redis.command("SET", "{" + config.baseKey + "}:compact:latest:v1:commit", "test");
  assert(str(redis.command("OBJECT", "ENCODING", compact).get()) == "listpack");
  const auto lengths = commandCount(redis, "hstrlen");
  XrmHashReader compactReader(config, "compact", 1024, 5, [](const auto&, const auto&, const auto&, const auto&) {});
  assert(compactReader.snapshot().rejected);
  assert(commandCount(redis, "hstrlen") == lengths);
  compactReader.reset();
  redis.command("CONFIG", "SET", "hash-max-listpack-value", "64");

  // An initial script denial is retried as a baseline, not claimed fresh.
  redis.command("ACL", "SETUSER", "xrm-hash-user", "on", ">xrm-hash-password", "resetkeys",
                "~{xrm-hash-test}:*", "-@all", "+type", "+object|encoding", "+memory|usage",
                "+hlen", "+hstrlen", "+hget", "+get", "+strlen");
  auto restricted = config; restricted.user = "xrm-hash-user"; restricted.password = "xrm-hash-password";
  std::mutex aclMutex;
  std::vector<uint64_t> aclValues;
  XrmHashReader acl(restricted, "waveform", 1024, 10,
      [&](const auto&, const auto&, const auto& entries, const auto&) {
        if (entries[0].second.empty()) return;
        std::lock_guard<std::mutex> guard(aclMutex);
        aclValues.push_back(std::stoull(entries[0].second.at("batch")));
      });
  assert(acl.snapshot().rejected);
  acl.start();
  redis.command("ACL", "SETUSER", "xrm-hash-user", "+eval");
  eventually([&] { std::lock_guard<std::mutex> guard(aclMutex); return !aclValues.empty(); });
  { std::lock_guard<std::mutex> guard(aclMutex); assert((aclValues == std::vector<uint64_t>{114})); }
  assert(acl.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(115);
  eventually([&] { std::lock_guard<std::mutex> guard(aclMutex); return aclValues.size() == 2; });
  assert(acl.freshnessProof() != std::chrono::steady_clock::time_point{});
  redis.command("ACL", "SETUSER", "xrm-hash-user", "-hstrlen");
  eventually([&] { return acl.status().readRejections >= 2; });
  assert(acl.freshnessProof() == std::chrono::steady_clock::time_point{});
  redis.command("ACL", "SETUSER", "xrm-hash-user", "+hstrlen");
  eventually([&] { std::lock_guard<std::mutex> guard(aclMutex); return aclValues.size() == 3; });
  assert(acl.freshnessProof() == std::chrono::steady_clock::time_point{});
  publish(116);
  eventually([&] { std::lock_guard<std::mutex> guard(aclMutex); return aclValues.size() == 4; });
  assert(acl.freshnessProof() != std::chrono::steady_clock::time_point{});
  acl.reset();
  redis.command("ACL", "DELUSER", "xrm-hash-user");

  publishTo("delayed", 1);
  DelayedProxy proxy(config.port);
  auto delayedConfig = config; delayedConfig.port = proxy.port();
  std::atomic<bool> observed{false};
  XrmHashReader delayed(delayedConfig, "delayed", 1024, 5,
      [&](const auto&, const auto&, const auto& entries, const auto&) {
        if (!entries[0].second.empty() && entries[0].second.at("batch") == "2") observed = true;
      });
  assert(delayed.snapshot().present());
  delayed.start();
  publishTo("delayed", 2);
  eventually([&] { return observed.load(); });
  const auto delayedProof = delayed.freshnessProof();
  assert(delayedProof != std::chrono::steady_clock::time_point{});
  assert(std::chrono::steady_clock::now() - delayedProof >= 200ms);
  delayed.reset();

  std::cout << "XRM hash bounds, freshness, outage, ACL and coalescing passed\n";
}
