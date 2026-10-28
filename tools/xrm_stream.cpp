// XRM socket -> Redis waveform companion. Opening the XRM socket can start
// acquisition. The XRM must already be configured to the supplied layout;
// this process does not write device PVs.
#include "redis_pvxs_ioc/xrm_acquisition.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
bool expired(Deadline deadline) { return stopping || Clock::now() >= deadline; }
void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
uint64_t integer(const std::string& text, uint64_t maximum) {
  uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result && result <= maximum,
          "numeric option outside bounds: " + text);
  return result;
}

struct Options {
  std::string xrmHost, redisHost = "127.0.0.1", base, prefix = "xrm";
  unsigned xrmPort = 4210, redisPort = 6379;
  size_t maxLength = 128;
  uint64_t durationMs = 0;
  XRMStreamConfig config;
};

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help") {
      std::cout << "XRM socket to Redis waveforms (numeric IPv4 addresses only).\n"
                   "WARNING: connecting to the XRM stream can start hardware acquisition.\n"
                   "Use only when explicitly authorized and after characterizing the device.\n"
                   "Channel selection and rate must match the device; they do not configure it.\n"
                   "Required: --xrm-host IP --base-key KEY --channels 1,2,...\n"
                   "          --sample-bytes 1..4 --sample-rate-hz N --samples-per-batch N\n"
                   "Optional: --xrm-port N --redis-host IP --redis-port N --key-prefix KEY\n"
                   "          --maxlen N --duration-ms N\n";
      std::exit(0);
    }
    require(index + 1 < argc, "missing option value: " + name);
    const std::string value = argv[++index];
    if (name == "--xrm-host") options.xrmHost = value;
    else if (name == "--xrm-port") options.xrmPort = unsigned(integer(value, 65535));
    else if (name == "--redis-host") options.redisHost = value;
    else if (name == "--redis-port") options.redisPort = unsigned(integer(value, 65535));
    else if (name == "--base-key") options.base = value;
    else if (name == "--key-prefix") options.prefix = value;
    else if (name == "--sample-bytes") options.config.sampleBytes = unsigned(integer(value, 4));
    else if (name == "--sample-rate-hz") options.config.sampleRateHz = integer(value, 100000000);
    else if (name == "--samples-per-batch") options.config.samplesPerBatch = size_t(integer(value, kXRMMaxBatchBytes));
    else if (name == "--maxlen") options.maxLength = size_t(integer(value, 1000000));
    else if (name == "--duration-ms") options.durationMs = integer(value, 86400000);
    else if (name == "--channels") {
      size_t start = 0;
      while (start < value.size()) {
        const auto end = value.find(',', start);
        options.config.channels.push_back(unsigned(integer(value.substr(start, end - start), 32)));
        if (end == std::string::npos) break;
        start = end + 1;
        require(start < value.size(), "empty channel after comma");
      }
    } else throw std::runtime_error("unknown option: " + name);
  }
  require(!options.xrmHost.empty() && !options.base.empty(), "--xrm-host and --base-key are required");
  // Validate layout, key names, and IPv4 addresses before any socket is opened.
  XRMFrameAssembler check(options.config);
  XRMBatch empty{options.config.samplesPerBatch, {}};
  for (auto channel : options.config.channels)
    empty.channels.push_back({channel, std::string(options.config.samplesPerBatch * options.config.sampleBytes, '\0')});
  (void)makeXRMRedisRecords(options.config, empty, {}, options.base, options.prefix, "check", 1, 1);
  in_addr address{};
  require(inet_pton(AF_INET, options.xrmHost.c_str(), &address) == 1 &&
          inet_pton(AF_INET, options.redisHost.c_str(), &address) == 1,
          "XRM and Redis hosts must be numeric IPv4 addresses");
  return options;
}

// Nonblocking sockets keep connect, read and write operations bounded. The
// short poll also allows shutdown on signals and on an optional deadline.
class Socket {
public:
  Socket() = default;
  ~Socket() { close(); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  void close() noexcept { if (fd_ >= 0) ::close(fd_); fd_ = -1; readBuffer_.clear(); }
  bool connected() const noexcept { return fd_ >= 0; }

  void connect(const std::string& host, unsigned port, Deadline runDeadline) {
    close();
    require(!expired(runDeadline), "connect stopped or run deadline expired");
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw std::runtime_error("socket creation failed");
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
      throw std::runtime_error("SO_NOSIGPIPE failed");
#endif
    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0)
      throw std::runtime_error("nonblocking socket setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
      throw std::runtime_error("invalid numeric IPv4 address");
    // In particular, do not initiate the XRM connection after a slow Redis
    // connection exhausted the run deadline or a stop was requested.
    require(!expired(runDeadline), "connect stopped or run deadline expired");
    const int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result < 0 && errno != EINPROGRESS) throw std::runtime_error("connect failed");
    if (result < 0) {
      if (!ready(POLLOUT, std::min(runDeadline, Clock::now() + 2s)))
        throw std::runtime_error("connect timed out or stopped");
      int error = 0; socklen_t length = sizeof(error);
      if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
        throw std::runtime_error("connect failed");
    }
  }

  // Returns -1 for a quiet socket, 0 for orderly close, positive for bytes.
  ssize_t receive(char* destination, size_t capacity, int timeoutMs, Deadline runDeadline) {
    if (!ready(POLLIN, std::min(runDeadline, Clock::now() + std::chrono::milliseconds(timeoutMs)))) return -1;
    if (expired(runDeadline)) return -1;
    const auto n = ::recv(fd_, destination, capacity, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
    if (n < 0) throw std::runtime_error("socket receive failed");
    return n;
  }
  void sendAll(const std::string& value, Deadline runDeadline) {
    size_t position = 0;
    const auto deadline = std::min(runDeadline, Clock::now() + 2s);
    while (position < value.size()) {
      if (!ready(POLLOUT, deadline) || expired(deadline)) throw std::runtime_error("Redis write timed out or stopped");
#ifdef MSG_NOSIGNAL
      const auto n = ::send(fd_, value.data() + position, value.size() - position, MSG_NOSIGNAL);
#else
      const auto n = ::send(fd_, value.data() + position, value.size() - position, 0);
#endif
      if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (n <= 0) throw std::runtime_error("Redis write failed");
      position += size_t(n);
    }
  }
  std::string line(Deadline deadline) {
    for (;;) {
      if (expired(deadline)) throw std::runtime_error("Redis response timed out or stopped");
      const auto end = readBuffer_.find("\r\n");
      if (end != std::string::npos) {
        auto result = readBuffer_.substr(0, end);
        readBuffer_.erase(0, end + 2);
        return result;
      }
      if (readBuffer_.size() > 1024) throw std::runtime_error("excessive Redis response line");
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (remaining <= 0) throw std::runtime_error("Redis response timed out");
      char block[4096];
      const auto n = receive(block, sizeof(block), int(remaining), deadline);
      if (n == 0) throw std::runtime_error("Redis disconnected during response");
      if (n > 0) readBuffer_.append(block, size_t(n));
      if (readBuffer_.size() > 65536) throw std::runtime_error("excessive Redis response");
    }
  }
private:
  bool ready(short events, Deadline deadline) {
    pollfd descriptor{fd_, events, 0};
    while (!expired(deadline)) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      // Poll in small bounded intervals so a stop is noticed even when the
      // signal arrived immediately before poll began.
      const auto timeoutMs = int(std::min<int64_t>(200, std::max<int64_t>(1, remaining)));
      const int result = ::poll(&descriptor, 1, timeoutMs);
      if (result > 0) {
        if (descriptor.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("socket error");
        return descriptor.revents & (events | POLLHUP);
      }
      if (result == 0) continue;
      if (errno != EINTR) throw std::runtime_error("socket poll failed");
      if (stopping) return false;
    }
    return false;
  }
  int fd_ = -1;
  std::string readBuffer_;
};

void appendCommand(std::string& output, const std::vector<std::string>& parts) {
  output += "*" + std::to_string(parts.size()) + "\r\n";
  for (const auto& part : parts) {
    output += "$" + std::to_string(part.size()) + "\r\n";
    output.append(part); output += "\r\n";
  }
}

void publish(Socket& redis, const std::vector<XRMRedisRecord>& records, size_t maxLength, Deadline runDeadline) {
  std::string requests;
  appendCommand(requests, {"MULTI"});
  for (const auto& record : records) {
    std::vector<std::string> arguments{"XADD", record.key, "MAXLEN", "=", std::to_string(maxLength), "*"};
    arguments.reserve(6 + record.fields.size() * 2);
    for (const auto& field : record.fields) { arguments.push_back(field.first); arguments.push_back(field.second); }
    appendCommand(requests, arguments);
  }
  appendCommand(requests, {"EXEC"});
  // On failure the transaction outcome may be unknown. The caller discards
  // the batch and closes both sockets; it must not replay the transaction.
  redis.sendAll(requests, runDeadline);
  const auto deadline = std::min(runDeadline, Clock::now() + 2s);
  require(redis.line(deadline) == "+OK", "Redis MULTI rejected");
  for (size_t i = 0; i < records.size(); ++i)
    require(redis.line(deadline) == "+QUEUED", "Redis XADD was not queued");
  require(redis.line(deadline) == "*" + std::to_string(records.size()), "Redis transaction failed");
  for (size_t i = 0; i < records.size(); ++i) {
    const auto header = redis.line(deadline);
    require(!header.empty() && header[0] == '$', "Redis XADD returned an error");
    const auto length = integer(header.substr(1), 64);
    const auto cursor = redis.line(deadline);
    require(cursor.size() == length && cursor.find('-') != std::string::npos, "invalid Redis stream ID response");
  }
}

std::string sessionId() {
  std::random_device device;
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 4; ++i) out << std::setw(8) << uint32_t(device());
  return out.str();
}

using Connector = std::function<void(Socket&, const std::string&, unsigned, Deadline)>;

int run(const Options& options, const Connector& connector =
    [](Socket& socket, const std::string& host, unsigned port, Deadline deadline) { socket.connect(host, port, deadline); }) {
  const auto start = Clock::now();
  const auto deadline = options.durationMs ? start + std::chrono::milliseconds(options.durationMs) : Deadline::max();
  XRMFrameAssembler assembler(options.config);
  Socket source, redis;
  const auto session = sessionId();
  uint64_t connection = 0, batchSequence = 0, published = 0, failed = 0, discarded = 0;
  unsigned retryMs = 250;
  char block[65536];
  const auto done = [&] { return expired(deadline); };
  while (!done()) {
    try {
      if (!redis.connected()) connector(redis, options.redisHost, options.redisPort, deadline);
      if (done()) break;
      if (!source.connected()) {
        if (done()) break;
        connector(source, options.xrmHost, options.xrmPort, deadline);
        ++connection;
      }
      if (done()) break;
      const auto count = source.receive(block, std::min(sizeof(block), assembler.remainingBytes()), 200, deadline);
      if (count < 0) continue;
      if (count == 0) throw std::runtime_error("XRM socket closed");
      assembler.consume(std::string_view(block, size_t(count)), [&](XRMBatch&& batch) {
        ++batchSequence;
        auto records = makeXRMRedisRecords(options.config, batch, {}, options.base, options.prefix,
                                           session, connection, batchSequence);
        try { publish(redis, records, options.maxLength, deadline); ++published; retryMs = 250; }
        catch (...) { ++failed; throw; }
      });
    } catch (const std::exception& error) {
      const auto partial = assembler.reset();
      discarded += partial;
      source.close(); redis.close();
      std::cerr << "xrm stream disconnected: " << error.what() << "; discarded " << partial
                << " partial batch bytes; ambiguous Redis batches are not replayed\n";
      const auto retryAt = Clock::now() + std::chrono::milliseconds(retryMs);
      while (!done() && Clock::now() < retryAt)
        std::this_thread::sleep_until(std::min({retryAt, deadline, Clock::now() + 50ms}));
      retryMs = std::min(5000u, retryMs * 2);
    }
  }
  discarded += assembler.reset();
  std::cerr << "xrm stream stopped: published=" << published << " failed_or_ambiguous=" << failed
            << " discarded_partial_bytes=" << discarded << " connections=" << connection << '\n';
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    auto options = parseOptions(argc, argv);
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    return run(options);
  } catch (const std::exception& error) {
    std::cerr << "xrm stream: " << error.what() << '\n';
    return 1;
  }
}
