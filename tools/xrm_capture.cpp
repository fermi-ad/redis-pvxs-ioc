#include "xrm_capture.h"
#include "xrm_event.h"

#include "hiredis.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <poll.h>
#include <random>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace xrm_capture {
namespace {
using Clock = std::chrono::steady_clock;
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
using Context = std::unique_ptr<redisContext, decltype(&redisFree)>;
constexpr unsigned leaseMs = 5000, renewMs = 1000;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
void keys(const YAML::Node& node, std::initializer_list<const char*> allowed, const std::string& path) {
  require(node.IsMap(), path + " must be a mapping");
  std::set<std::string> seen;
  for (const auto& pair : node) {
    require(pair.first.IsScalar(), path + " has a non-scalar field");
    const auto key = pair.first.as<std::string>();
    require(seen.insert(key).second, path + " has duplicate field " + key);
    require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), path + " has unknown field " + key);
  }
}
std::string text(const YAML::Node& node, const std::string& field) {
  require(node.IsScalar(), field + " is required and must be scalar");
  auto value = node.as<std::string>();
  require(!value.empty() && value.size() <= 512, field + " length is invalid");
  for (unsigned char ch : value)
    require(ch > 0x20 && ch < 0x7f, field + " must contain printable ASCII without spaces");
  return value;
}
unsigned number(const YAML::Node& node, unsigned fallback, unsigned minimum,
                unsigned maximum, const std::string& field) {
  if (!node) return fallback;
  auto valueText = text(node, field);
  unsigned value = 0;
  auto parsed = std::from_chars(valueText.data(), valueText.data() + valueText.size(), value);
  require(parsed.ec == std::errc{} && parsed.ptr == valueText.data() + valueText.size() &&
          value >= minimum && value <= maximum, field + " must be an unsigned decimal integer within its bound");
  return value;
}
std::string ipv4(const std::string& host) {
  in_addr address{};
  require(inet_pton(AF_INET, host.c_str(), &address) == 1, "capture device.host requires a numeric IPv4 address");
  const auto bits = ntohl(address.s_addr);
  require((bits >> 24) != 0 && (bits >> 24) < 224 && (bits & 0xffu) != 255,
          "capture device.host requires a unicast IPv4 address");
  char buffer[INET_ADDRSTRLEN];
  require(inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) != nullptr, "cannot format capture IPv4 address");
  return buffer;
}
std::string readConfig(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot read capture configuration " + path);
  std::string result;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof(buffer));
    auto count = static_cast<size_t>(input.gcount());
    require(count <= 1024u * 1024u - result.size(), "capture configuration exceeds 1 MiB");
    result.append(buffer, count);
  }
  require(input.eof(), "cannot finish reading capture configuration " + path);
  return result;
}

// The Redis lease is separate from demux checkpoints. Its token fences each
// XADD in the same server invocation, including recovery after Redis restart.
const std::string claimScript = R"LUA(
local kind = redis.call('TYPE',KEYS[1]).ok
if kind ~= 'none' and kind ~= 'string' then return redis.error_reply('capture lease must be a string') end
local owner = redis.call('GET',KEYS[1])
if owner and owner ~= ARGV[1] then return 0 end
redis.call('SET',KEYS[1],ARGV[1],'PX',ARGV[2])
return 1
)LUA";
const std::string publishScript = R"LUA(
local kind = redis.call('TYPE',KEYS[1]).ok
if kind ~= 'none' and kind ~= 'string' then return redis.error_reply('capture lease must be a string') end
if redis.call('GET',KEYS[1]) ~= ARGV[1] or redis.call('PTTL',KEYS[1]) <= 0 then return {'lease_lost'} end
local streamKind = redis.call('TYPE',KEYS[2]).ok
if streamKind ~= 'none' and streamKind ~= 'stream' then return redis.error_reply('capture source must be a stream') end
return {'published',redis.call('XADD',KEYS[2],'MAXLEN',ARGV[2],'*','_',ARGV[3])}
)LUA";
const std::string releaseScript = R"LUA(
if redis.call('TYPE',KEYS[1]).ok == 'string' and redis.call('GET',KEYS[1]) == ARGV[1] then
  return redis.call('DEL',KEYS[1])
end
return 0
)LUA";

struct CommandFailure : std::runtime_error {
  // True only after the complete command was submitted and no valid reply
  // established its result. The caller must never replay this pulse.
  bool uncertain;
  CommandFailure(const std::string& message, bool value) : std::runtime_error(message), uncertain(value) {}
};

Reply command(redisContext* context, const std::vector<std::string>& args,
              unsigned timeoutMs, const std::atomic<bool>& stopping) {
  std::vector<const char*> argv;
  std::vector<size_t> sizes;
  for (const auto& arg : args) { argv.push_back(arg.data()); sizes.push_back(arg.size()); }
  char* formatted = nullptr;
  auto length = redisFormatCommandArgv(&formatted, static_cast<int>(args.size()), argv.data(), sizes.data());
  require(length >= 0 && formatted, "cannot format capture Redis command");
  // redisFormatCommandArgv uses the hiredis allocator; pair its public free.
  std::unique_ptr<char, decltype(&redisFreeCommand)> bytes(formatted, &redisFreeCommand);
  size_t sent = 0;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  const auto fail = [&](const std::string& reason) -> void {
    throw CommandFailure(reason, sent == static_cast<size_t>(length));
  };
  const auto ready = [&](short event) {
    for (;;) {
      if (stopping) fail("capture worker stopping");
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (remaining <= 0) fail("Redis command deadline exceeded");
      pollfd fd{context->fd, event, 0};
      auto result = ::poll(&fd, 1, static_cast<int>(std::min<int64_t>(remaining, 50)));
      if (result < 0 && errno == EINTR) continue;
      if (result < 0) fail("Redis readiness polling failed: " + std::string(std::strerror(errno)));
      if (result > 0) {
        // A final readable reply may arrive together with HUP. Consume it.
        if (fd.revents & event) return;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) fail("Redis connection closed");
      }
    }
  };
  while (sent < static_cast<size_t>(length)) {
    ready(POLLOUT);
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    auto count = ::send(context->fd, bytes.get() + sent, static_cast<size_t>(length) - sent, flags);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) fail("Redis write failed: " + std::string(count ? std::strerror(errno) : "connection closed"));
    sent += static_cast<size_t>(count);
  }
  void* object = nullptr;
  while (!object) {
    if (stopping) fail("capture worker stopping");
    if (Clock::now() >= deadline) fail("Redis command deadline exceeded");
    if (redisGetReplyFromReader(context, &object) != REDIS_OK) fail("invalid Redis response");
    if (object) break;
    ready(POLLIN);
    if (redisBufferRead(context) != REDIS_OK) fail("Redis read failed: " + std::string(context->errstr));
  }
  Reply reply(static_cast<redisReply*>(object), &freeReplyObject);
  // The script preflights types and fencing before its sole mutation. An
  // explicit Redis error is a known rejection, not an uncertain dispatch.
  if (reply->type == REDIS_REPLY_ERROR)
    throw CommandFailure("Redis rejected capture operation: " + std::string(reply->str, reply->len), false);
  return reply;
}
bool replyEquals(const redisReply* value, const char* expected) {
  return value && value->type == REDIS_REPLY_STRING &&
      value->len == std::strlen(expected) && std::memcmp(value->str, expected, value->len) == 0;
}
struct Socket {
  int fd = -1;
  explicit Socket(int value = -1) : fd(value) {}
  ~Socket() { if (fd >= 0) ::close(fd); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
};

std::string writerToken() {
  std::random_device random;
  std::ostringstream token;
  token << std::hex << static_cast<uint64_t>(Clock::now().time_since_epoch().count()) << '-' << ::getpid();
  for (unsigned i = 0; i < 4; ++i) token << '-' << random();
  return token.str();
}
int openLogSink() {
  // Reopening the descriptor creates a private open-file description on Linux,
  // so O_NONBLOCK does not change stderr for the inventory/demux threads.
#ifdef O_CLOEXEC
  constexpr int flags = O_WRONLY | O_NONBLOCK | O_APPEND | O_CLOEXEC;
#else
  constexpr int flags = O_WRONLY | O_NONBLOCK | O_APPEND;
#endif
  int fd = ::open("/proc/self/fd/2", flags);
  if (fd < 0) fd = ::open("/dev/fd/2", flags);
  if (fd < 0) {
    const auto original = ::fcntl(STDERR_FILENO, F_GETFL, 0);
    if (original >= 0 && (original & O_NONBLOCK)) fd = ::dup(STDERR_FILENO);
  }
  return fd;  // Logging is best effort; never fall back to a blocking sink.
}
}  // namespace

Config parseConfig(const std::string& yaml, const xrm_demux::Config& demux) {
  require(yaml.size() <= 1024u * 1024u, "capture configuration exceeds 1 MiB");
  const auto documents = YAML::LoadAll(yaml);
  require(documents.size() == 1, "capture configuration must contain exactly one YAML document");
  const auto root = documents.front();
  keys(root, {"schema_version", "source_stream", "device", "reconnect", "queue_limit_bytes"}, "capture");
  require(text(root["schema_version"], "capture.schema_version") == "1", "unsupported capture schema_version");
  const auto stream = text(root["source_stream"], "capture.source_stream");
  Config result;
  const auto found = std::find_if(demux.sources.begin(), demux.sources.end(),
                               [&](const auto& source) { return source.stream == stream; });
  require(found != demux.sources.end(), "capture.source_stream must name a configured demux source");
  result.source = *found;
  require(found->format == "acq4xx-es-int16-le" && found->channels.size() == 16 &&
          found->samplesPerChannel == samplesPerChannel,
          "capture supports only the 16-channel, 16383-sample acq4xx-es-int16-le profile");
  require(found->payloadField == "_", "capture demux source payload_field must be '_'");
  require(found->timestampMode == "absent", "capture demux source timestamp mode must be absent");
  require(found->inputRetention > 0 && found->inputRetention == found->retention,
          "capture demux source requires explicit input_retention equal to retention");
  const auto leaseKey = stream + ":capture:lease:v1";
  const auto disjoint = [&](const std::string& key) {
    require(key != leaseKey, "capture lease key collides with a configured demux key: " + leaseKey);
  };
  const auto sourceKeys = [&](const xrm_demux::Source& source) {
    disjoint(source.stream); disjoint(source.checkpoint);
    for (const auto& channel : source.channels) { disjoint(channel.raw); disjoint(channel.volts); }
  };
  for (const auto& source : demux.sources) sourceKeys(source);
  for (const auto& conversion : demux.currentConversions) {
    sourceKeys(conversion);
    disjoint(conversion.output); disjoint(conversion.setting); disjoint(conversion.applied);
  }
  keys(root["device"], {"host", "port"}, "capture.device");
  result.host = ipv4(text(root["device"]["host"], "capture.device.host"));
  require(root["device"]["port"].IsScalar(), "capture.device.port must be explicit");
  result.port = number(root["device"]["port"], 4210, 1, 65535, "capture.device.port");
  if (root["reconnect"]) {
    keys(root["reconnect"], {"initial_ms", "max_ms"}, "capture.reconnect");
    result.initialReconnectMs = number(root["reconnect"]["initial_ms"], 1000, 1, 30000, "capture.reconnect.initial_ms");
    result.maxReconnectMs = number(root["reconnect"]["max_ms"], 30000, result.initialReconnectMs, 300000,
                                   "capture.reconnect.max_ms");
  }
  result.queueLimitBytes = number(root["queue_limit_bytes"], 32u * 1024u * 1024u, pulseBytes,
                                 64u * 1024u * 1024u, "capture.queue_limit_bytes");
  result.redis = demux;
  // Retain only Redis connection policy, rather than another copy of all lanes.
  result.redis.sources.clear();
  result.redis.currentConversions.clear();
  return result;
}
Config loadConfig(const std::string& path, const xrm_demux::Config& demux) {
  return parseConfig(readConfig(path), demux);
}

void Framer::discard(size_t count) {
  discarded_ += count;
  searching_ += count;
  if (searching_ >= 2 * pulseBytes) {
    faulted_ = true;
    throw FramingError("no supported complete ES pulse within two pulse lengths; capture fault latched");
  }
}
void Framer::process(const Sink& sink) {
  size_t cursor = 0;
  try {
    while (buffer_.size() - cursor >= 16) {
      if (!xrm_demux::event_detail::hasEventPrefix(buffer_, cursor)) {
        ++cursor;
        discard(1);
        continue;
      }
      if (buffer_.size() - cursor < pulseBytes) break;
      auto pulse = buffer_.substr(cursor, pulseBytes);
      try {
        // Reuse the decoder's magic, paired-counter, and embedded-ES checks.
        (void)xrm_demux::decodeEventPulse(pulse, 16, samplesPerChannel);
      } catch (const std::invalid_argument&) {
        ++rejected_;
        ++cursor;
        discard(1);
        continue;
      }
      cursor += pulseBytes;
      searching_ = 0;
      sink(std::move(pulse));
    }
  } catch (const FramingError&) {
    if (cursor) buffer_.erase(0, cursor);
    throw;
  }
  if (cursor) buffer_.erase(0, cursor);
}
void Framer::feed(const char* bytes, size_t count, const Sink& sink) {
  if (faulted_) throw FramingError("capture framing fault is latched");
  while (count) {
    auto amount = std::min(count, pulseBytes - buffer_.size());
    buffer_.append(bytes, amount);
    bytes += amount;
    count -= amount;
    process(sink);
  }
}
void Framer::reset() {
  discarded_ += buffer_.size();
  buffer_.clear();
  searching_ = 0;
}

struct Worker::Impl {
  Config config;
  const std::string endpoint, leaseKey, token;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> logDrops{0};
  mutable std::mutex mutex;
  std::mutex logMutex;
  std::condition_variable wake;
  std::thread reader, writer;
  Socket logSink;
  size_t maxLogBytes = 512;
  std::deque<std::string> queue;
  Status status;
  Clock::time_point lastReceive{}, lastPublish{}, leaseExpiry{};
  bool leaseHeld = false, captureFault = false;

  explicit Impl(Config value) : config(std::move(value)),
      endpoint(config.host + ":" + std::to_string(config.port)),
      leaseKey(config.source.stream + ":capture:lease:v1"), token(writerToken()), logSink(openLogSink()) {
    if (logSink.fd >= 0) {
      const auto bound = ::fpathconf(logSink.fd, _PC_PIPE_BUF);
      if (bound > 0) maxLogBytes = std::min<size_t>(static_cast<size_t>(bound), 2048);
    }
  }

  void log(const char* severity, const char* event, const std::string& details = {}) {
    std::ostringstream line;
    line << "xrm-proxy: capture severity=" << severity << " event=" << event << " endpoint=" << endpoint;
    if (!details.empty()) line << ' ' << details;
    line << '\n';
    auto bytes = line.str();
    if (bytes.size() > maxLogBytes) { bytes.resize(maxLogBytes); bytes.back() = '\n'; }
    std::lock_guard<std::mutex> lock(logMutex);
    const auto count = logSink.fd < 0 ? -1 : ::write(logSink.fd, bytes.data(), bytes.size());
    if (count < 0 || static_cast<size_t>(count) != bytes.size()) ++logDrops;
  }
  static std::string errorField(const std::string& error) {
    std::ostringstream result;
    result << "error=" << std::quoted(error.substr(0, 512));
    return result.str();
  }
  void state(const std::string& value, const std::string& error = {}) {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (captureFault && value != "fault") return;
      changed = status.state != value || status.error != error;
      status.state = value;
      status.error = error;
    }
    if (changed) report();
  }
  void lease(const std::string& value, Clock::time_point validUntil = {}) {
    bool changed;
    {
      std::lock_guard<std::mutex> lock(mutex);
      changed = status.leaseState != value;
      status.leaseState = value;
      leaseHeld = value == "held";
      leaseExpiry = leaseHeld ? validUntil : Clock::time_point{};
    }
    wake.notify_all();
    if (changed) report();
  }
  Status snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    auto result = status;
    result.logDrops = logDrops.load();
    auto now = Clock::now();
    if (lastReceive != Clock::time_point{})
      result.lastReceiveAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReceive).count();
    if (lastPublish != Clock::time_point{})
      result.lastPublishAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPublish).count();
    return result;
  }
  void report() {
    auto value = snapshot();
    std::ostringstream line;
    line << "state=" << value.state << " running=" << (value.running ? "true" : "false")
         << " received=" << value.received << " published=" << value.published
         << " reconnects=" << value.reconnects << " queueDepth=" << value.queueDepth
         << " queueBytes=" << value.queueBytes << " queueHighWater=" << value.queueHighWater
         << " localDrops=" << value.localDrops << " rejects=" << value.rejects
         << " discardedBytes=" << value.discardedBytes << " redisFailures=" << value.redisFailures
         << " leaseState=" << value.leaseState << " uncertainWrites=" << value.uncertainWrites
         << " logDrops=" << value.logDrops
         << " lastReceiveAgeMs=" << value.lastReceiveAgeMs << " lastPublishAgeMs=" << value.lastPublishAgeMs;
    if (!value.error.empty()) line << ' ' << errorField(value.error);
    log("INFO", "status", line.str());
  }
  void wait(unsigned milliseconds) {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [&] { return stopping.load(); });
  }
  bool waitForLease() {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait(lock, [&] { return stopping || (leaseHeld && Clock::now() < leaseExpiry); });
    return !stopping;
  }
  void enqueue(std::string&& pulse) {
    bool firstDrop = false;
    uint64_t drops = 0;
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++status.received;
      lastReceive = Clock::now();
      if (stopping) { ++status.localDrops; return; }
      while (status.queueBytes + pulse.size() > config.queueLimitBytes) {
        firstDrop = firstDrop || status.localDrops == 0;
        status.queueBytes -= queue.front().size();
        queue.pop_front();
        ++status.localDrops;
      }
      drops = status.localDrops;
      status.queueBytes += pulse.size();
      queue.push_back(std::move(pulse));
      status.queueDepth = queue.size();
      status.queueHighWater = std::max(status.queueHighWater, status.queueDepth);
    }
    wake.notify_all();
    if (firstDrop) log("ERROR", "queue_drop", "localDrops=" + std::to_string(drops) +
                       " policy=drop_oldest_queued_complete_pulse");
  }
  void frameStatus(const Framer& framer) {
    std::lock_guard<std::mutex> lock(mutex);
    status.rejects = framer.rejected();
    status.discardedBytes = framer.discardedBytes();
  }
  void connectDevice(Socket& socket) {
    socket.fd = ::socket(AF_INET, SOCK_STREAM, 0);
    require(socket.fd >= 0, "cannot create acquisition socket: " + std::string(std::strerror(errno)));
    auto flags = ::fcntl(socket.fd, F_GETFL, 0);
    require(flags >= 0 && ::fcntl(socket.fd, F_SETFL, flags | O_NONBLOCK) == 0,
            "cannot set acquisition socket nonblocking");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    require(inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) == 1, "invalid capture address");
    auto result = ::connect(socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result == 0) return;
    require(errno == EINPROGRESS, "acquisition connect failed: " + std::string(std::strerror(errno)));
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!stopping && Clock::now() < deadline) {
      pollfd fd{socket.fd, POLLOUT, 0};
      result = ::poll(&fd, 1, 50);
      if (result < 0 && errno == EINTR) continue;
      require(result >= 0, "acquisition connect polling failed");
      if (!result) continue;
      int error = 0;
      socklen_t size = sizeof(error);
      require(::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0,
              "cannot read acquisition connection result");
      require(!error, "acquisition connect failed: " + std::string(std::strerror(error)));
      return;
    }
    require(!stopping, "capture worker stopping");
    throw std::runtime_error("acquisition connection deadline exceeded");
  }
  void readLoop() noexcept {
    Framer framer;
    unsigned delay = config.initialReconnectMs;
    uint64_t attempts = 0;
    bool recovery = false;
    try {
      state("awaiting_lease");
      while (!stopping) {
        if (!waitForLease()) break;
        if (attempts++) { std::lock_guard<std::mutex> lock(mutex); ++status.reconnects; }
        try {
          state("connecting");
          Socket socket;
          connectDevice(socket);
          state("waiting_for_pulse");
          bool validated = false;
          char bytes[65536];
          while (!stopping) {
            pollfd fd{socket.fd, POLLIN, 0};
            auto result = ::poll(&fd, 1, 50);
            if (result < 0 && errno == EINTR) continue;
            require(result >= 0, "acquisition receive polling failed: " + std::string(std::strerror(errno)));
            if (!result) continue;  // Ordinary silence is not a fault.
            require(!(fd.revents & POLLNVAL), "acquisition socket became invalid");
            auto count = ::recv(socket.fd, bytes, sizeof(bytes), 0);
            if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            require(count > 0, count == 0 ? "acquisition peer closed connection (EOF)" :
                    "acquisition receive failed: " + std::string(std::strerror(errno)));
            framer.feed(bytes, static_cast<size_t>(count), [&](std::string&& pulse) {
              enqueue(std::move(pulse));
              if (!validated) {
                validated = true;
                delay = config.initialReconnectMs;
                if (recovery) log("INFO", "recovery", "reason=validated_complete_pulse");
                recovery = false;
                state("streaming");
              }
            });
            frameStatus(framer);
          }
        } catch (const FramingError& error) {
          frameStatus(framer);
          {
            std::lock_guard<std::mutex> lock(mutex);
            captureFault = true;
          }
          log("ERROR", "framing_fault", errorField(error.what()));
          state("fault", error.what());
          break;
        } catch (const std::exception& error) {
          framer.reset();  // Never splice bytes across connection generations.
          frameStatus(framer);
          if (stopping) break;
          log("ERROR", "disconnect", errorField(error.what()));
          recovery = true;
          state("backoff", error.what());
          log("INFO", "retry", "delay_ms=" + std::to_string(delay));
          wait(delay);
          delay = std::min(config.maxReconnectMs, delay > config.maxReconnectMs / 2 ? config.maxReconnectMs : delay * 2);
        }
      }
      framer.reset();
      frameStatus(framer);
    } catch (const std::exception& error) {
      log("ERROR", "reader_fault", errorField(error.what()));
      { std::lock_guard<std::mutex> lock(mutex); captureFault = true; }
      state("fault", error.what());
    }
  }
  Context connectRedis() {
    const auto ms = config.redis.connectTimeoutMs;
    const timeval timeout{static_cast<time_t>(ms / 1000), static_cast<suseconds_t>((ms % 1000) * 1000)};
    Context context(redisConnectWithTimeout(config.redis.host.c_str(), config.redis.port, timeout), &redisFree);
    require(context && !context->err, context ? "Redis connection failed: " + std::string(context->errstr) : "Redis allocation failed");
    auto flags = ::fcntl(context->fd, F_GETFL, 0);
    require(flags >= 0 && ::fcntl(context->fd, F_SETFL, flags | O_NONBLOCK) == 0, "cannot set Redis socket nonblocking");
    context->flags &= ~REDIS_BLOCK;
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    require(::setsockopt(context->fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0,
            "cannot suppress Redis socket SIGPIPE");
#endif
    if (config.redis.db)
      command(context.get(), {"SELECT", std::to_string(config.redis.db)}, config.redis.commandTimeoutMs, stopping);
    return context;
  }
  void writeLoop() noexcept {
    Context context(nullptr, &redisFree);
    std::optional<std::string> inflight;
    auto nextRenew = Clock::now(), nextReport = Clock::now();
    std::string previousError;
    bool releaseConfirmed = false;
    try {
      while (!stopping) {
        if (Clock::now() >= nextReport) {
          report();
          nextReport = Clock::now() + std::chrono::seconds(5);
        }
        bool publishing = false;
        try {
          if (!context) context = connectRedis();
          if (Clock::now() >= nextRenew) {
            const auto claimStarted = Clock::now();
            auto result = command(context.get(), {"EVAL", claimScript, "1", leaseKey, token, std::to_string(leaseMs)},
                                  config.redis.commandTimeoutMs, stopping);
            require(result->type == REDIS_REPLY_INTEGER && (result->integer == 0 || result->integer == 1),
                    "unexpected capture lease reply");
            nextRenew = Clock::now() + std::chrono::milliseconds(renewMs);
            if (!result->integer) {
              if (snapshot().leaseState != "contended") log("ERROR", "lease", "state=contended");
              lease("contended");
              wait(renewMs);
              continue;
            }
            // Redis starts PX while executing SET, before the reply arrives.
            // Request start with a small margin is conservative for new socket
            // connections even when the acknowledgement was delayed.
            lease("held", claimStarted + std::chrono::milliseconds(leaseMs - 100));
            previousError.clear();
          }
          if (!inflight) {
            std::unique_lock<std::mutex> lock(mutex);
            if (queue.empty()) {
              wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return stopping || !queue.empty(); });
              continue;
            }
            inflight = std::move(queue.front());
            queue.pop_front();
            status.queueBytes -= inflight->size();
            status.queueDepth = queue.size();
          }
          publishing = true;
          auto result = command(context.get(), {"EVAL", publishScript, "2", leaseKey, config.source.stream,
                                token, std::to_string(config.source.inputRetention), *inflight},
                                config.redis.commandTimeoutMs, stopping);
          if (result->type == REDIS_REPLY_ARRAY && result->elements == 1 && replyEquals(result->element[0], "lease_lost")) {
            lease("expired");
            nextRenew = Clock::now();
            continue;  // The fence established that no XADD occurred.
          }
          if (result->type != REDIS_REPLY_ARRAY || result->elements != 2 ||
              !replyEquals(result->element[0], "published") || result->element[1]->type != REDIS_REPLY_STRING)
            throw CommandFailure("unrecognized capture publication reply", true);
          {
            std::lock_guard<std::mutex> lock(mutex);
            ++status.published;
            lastPublish = Clock::now();
          }
          inflight.reset();
          previousError.clear();
        } catch (const std::exception& error) {
          const auto* failure = dynamic_cast<const CommandFailure*>(&error);
          bool uncertain = false;
          {
            std::lock_guard<std::mutex> lock(mutex);
            ++status.redisFailures;
            if (publishing && failure && failure->uncertain) {
              ++status.uncertainWrites;
              uncertain = true;
              inflight.reset();  // Never replay a possibly committed generated-ID XADD.
            }
          }
          context.reset();
          lease("unknown");
          nextRenew = Clock::now();
          if (stopping) break;
          if (uncertain) log("ERROR", "uncertain_write", "action=drop_without_replay");
          if (previousError != error.what()) log("ERROR", "redis_failure", errorField(error.what()));
          previousError = error.what();
          wait(config.redis.retryMs);
        }
      }
      if (context) {
        const std::atomic<bool> finishing{false};
        try {
          auto result = command(context.get(), {"EVAL", releaseScript, "1", leaseKey, token}, 100, finishing);
          releaseConfirmed = result->type == REDIS_REPLY_INTEGER && result->integer == 1;
        }
        catch (...) {}  // The lease expires even if bounded release is unavailable.
      }
    } catch (const std::exception& error) {
      log("ERROR", "writer_fault", errorField(error.what()));
      { std::lock_guard<std::mutex> lock(mutex); captureFault = true; }
      state("fault", error.what());
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      status.localDrops += queue.size() + (inflight ? 1 : 0);
      queue.clear();
      status.queueDepth = status.queueBytes = 0;
    }
    lease(releaseConfirmed ? "released" : "unknown");
  }
  void start() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      require(!status.running, "capture worker already started");
      status.running = true;
      status.state = "starting";
      stopping = false;
    }
    try {
      writer = std::thread([this] { writeLoop(); });
      reader = std::thread([this] { readLoop(); });
    } catch (...) {
      stopping = true;
      wake.notify_all();
      if (writer.joinable()) writer.join();
      { std::lock_guard<std::mutex> lock(mutex); status.running = false; }
      throw;
    }
  }
  void stop() noexcept {
    stopping = true;
    wake.notify_all();
    if (reader.joinable()) reader.join();
    if (writer.joinable()) writer.join();
    bool changed;
    {
      std::lock_guard<std::mutex> lock(mutex);
      changed = status.running;
      status.running = false;
      if (!captureFault) status.state = "stopped";
    }
    if (changed) report();
  }
};

Worker::Worker(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Worker::~Worker() { stop(); }
void Worker::start() { impl_->start(); }
void Worker::stop() noexcept { impl_->stop(); }
Status Worker::status() const { return impl_->snapshot(); }

}  // namespace xrm_capture
