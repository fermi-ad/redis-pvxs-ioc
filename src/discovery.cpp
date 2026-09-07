#include "redis_pvxs_ioc/discovery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace redis_pvxs_ioc {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<uint8_t>;
constexpr uint16_t magic = 0x5243;
struct Interrupted {};

struct Socket {
  int fd = -1;
  explicit Socket(int value = -1) : fd(value) {}
  ~Socket() { if (fd >= 0) ::close(fd); }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
};

[[noreturn]] void socketError(const char* operation) {
  throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

void nonblocking(int fd) {
  if (fd < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    socketError("configure discovery socket");
#ifdef SO_NOSIGPIPE
  const int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

void append16(Bytes& out, uint16_t value) {
  out.push_back(value >> 8); out.push_back(value & 0xff);
}
void append32(Bytes& out, uint32_t value) {
  append16(out, value >> 16); append16(out, value & 0xffff);
}
uint16_t read16(const uint8_t* bytes) { return (uint16_t(bytes[0]) << 8) | bytes[1]; }
uint32_t read32(const uint8_t* bytes) { return (uint32_t(read16(bytes)) << 16) | read16(bytes + 2); }
void appendText(Bytes& out, const std::string& value) { out.insert(out.end(), value.begin(), value.end()); }

void validateText(const std::string& value, size_t maximum, const char* field, bool allowEmpty = false) {
  if ((!allowEmpty && value.empty()) || value.size() > maximum || value.find('\0') != std::string::npos)
    throw std::runtime_error(std::string("discovery ") + field + " is empty, too long, or contains NUL");
}

void message(Bytes& wire, uint16_t id, const Bytes& body, uint64_t maximum) {
  if (wire.size() > maximum || body.size() + 8u > maximum - wire.size())
    throw std::runtime_error("discovery catalog exceeds max_bytes");
  append16(wire, magic); append16(wire, id); append32(wire, body.size());
  wire.insert(wire.end(), body.begin(), body.end());
}

void info(Bytes& wire, uint32_t id, const std::string& key, const std::string& value, uint64_t maximum) {
  validateText(key, 255, "property name");
  validateText(value, 65535, "property value", true);
  // RecCeiver decodes text before splitting its fields. ASCII property names
  // preserve byte/character offsets; values may be UTF-8.
  if (std::any_of(key.begin(), key.end(), [](unsigned char ch) { return ch >= 128; }))
    throw std::runtime_error("discovery property names must be ASCII");
  Bytes body;
  append32(body, id); body.push_back(key.size()); body.push_back(0); append16(body, value.size());
  appendText(body, key); appendText(body, value);
  message(wire, 6, body, maximum);
}
} // namespace

std::shared_ptr<const DiscoveryCatalog> prepareDiscoveryCatalog(
    const DiscoveryConfig& limits, uint64_t generation,
    const std::map<std::string, std::string>& identity,
    const std::vector<DiscoveryRecord>& records) {
  auto catalog = std::make_shared<DiscoveryCatalog>();
  catalog->generation = generation;
  for (const auto& property : identity) info(catalog->wire, 0, property.first, property.second, limits.maxBytes);
  std::set<std::string> names;
  uint32_t id = 0;
  for (const auto& record : records) {
    if (catalog->records + catalog->aliases >= limits.maxRecords)
      throw std::runtime_error("discovery catalog exceeds max_records");
    validateText(record.name, 65535, "PV name");
    validateText(record.type, 255, "type");
    if (!names.insert(record.name).second) throw std::runtime_error("duplicate discovery PV: " + record.name);
    Bytes body;
    append32(body, ++id); body.push_back(0); body.push_back(record.type.size()); append16(body, record.name.size());
    appendText(body, record.type); appendText(body, record.name);
    message(catalog->wire, 3, body, limits.maxBytes);
    ++catalog->records;
    for (const auto& alias : record.aliases) {
      if (catalog->records + catalog->aliases >= limits.maxRecords)
        throw std::runtime_error("discovery catalog exceeds max_records");
      validateText(alias, 65535, "alias");
      if (!names.insert(alias).second) throw std::runtime_error("duplicate discovery PV: " + alias);
      body.clear();
      append32(body, id); body.push_back(1); body.push_back(0); append16(body, alias.size()); appendText(body, alias);
      message(catalog->wire, 3, body, limits.maxBytes);
      ++catalog->aliases;
    }
    for (const auto& property : record.properties)
      info(catalog->wire, id, property.first, property.second, limits.maxBytes);
  }
  message(catalog->wire, 5, Bytes(4, 0), limits.maxBytes);
  return catalog;
}

struct DiscoveryPublisher::Impl {
  const DiscoveryConfig config;
  Socket wakeRead, wakeWrite;
  std::atomic<bool> stopping{false};
  std::atomic<uint64_t> epoch{0};
  mutable std::mutex mutex;
  std::shared_ptr<const DiscoveryCatalog> latest;
  DiscoveryStatus current;
  std::thread worker;

  explicit Impl(DiscoveryConfig settings) : config(std::move(settings)) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) socketError("discovery wakeup pair");
    wakeRead.fd = pair[0]; wakeWrite.fd = pair[1];
    nonblocking(wakeRead.fd); nonblocking(wakeWrite.fd);
    current.state = "idle";
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    stopping = true;
    wake();
    if (worker.joinable()) worker.join();
  }
  void wake() {
    const char byte = 1;
    (void)::send(wakeWrite.fd, &byte, 1, 0); // A full wakeup buffer already wakes the worker.
  }
  void drain() {
    std::array<char, 128> bytes;
    while (::recv(wakeRead.fd, bytes.data(), bytes.size(), 0) > 0) {}
  }
  void check(uint64_t expected) {
    if (stopping || epoch.load() != expected) throw Interrupted{};
  }
  void wait(int fd, short events, Clock::time_point deadline, uint64_t expected) {
    for (;;) {
      check(expected);
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (remaining <= 0) throw std::runtime_error("discovery network deadline exceeded");
      pollfd fds[]{{wakeRead.fd, POLLIN, 0}, {fd, events, 0}};
      const int result = poll(fds, fd < 0 ? 1 : 2, int(std::min<int64_t>(remaining, 60000)));
      if (result < 0) { if (errno == EINTR) continue; socketError("discovery poll"); }
      if (fds[0].revents) { drain(); check(expected); }
      if (fd >= 0 && fds[1].revents) return;
    }
  }
  void delay(uint32_t millis, uint64_t expected) {
    if (!millis) { check(expected); return; }
    try { wait(-1, 0, Clock::now() + std::chrono::milliseconds(millis), expected); }
    catch (const std::runtime_error&) {} // Expiration is the intended end of a delay.
  }
  void send(int fd, const Bytes& data, Clock::time_point deadline, uint64_t expected) {
    size_t offset = 0;
    while (offset != data.size()) {
      wait(fd, POLLOUT, deadline, expected);
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto count = ::send(fd, data.data() + offset, std::min<size_t>(16384, data.size() - offset), flags);
      if (count > 0) offset += size_t(count);
      else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
      else socketError("discovery send");
    }
  }
  void receive(int fd, uint8_t* bytes, size_t size, Clock::time_point deadline, uint64_t expected) {
    size_t offset = 0;
    while (offset != size) {
      wait(fd, POLLIN, deadline, expected);
      const auto count = ::recv(fd, bytes + offset, size - offset, 0);
      if (count == 0) throw std::runtime_error("RecCeiver disconnected");
      if (count > 0) offset += size_t(count);
      else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      else socketError("discovery receive");
    }
  }
  std::pair<uint16_t, Bytes> receiveMessage(int fd, Clock::time_point deadline, uint64_t expected) {
    std::array<uint8_t, 8> header;
    receive(fd, header.data(), header.size(), deadline, expected);
    const auto length = read32(header.data() + 4);
    if (read16(header.data()) != magic || length > 64)
      throw std::runtime_error("invalid or oversized RecCeiver control message");
    Bytes body(length);
    receive(fd, body.data(), body.size(), deadline, expected);
    return {read16(header.data() + 2), std::move(body)};
  }
  void state(const std::string& value) {
    std::lock_guard<std::mutex> guard(mutex); current.state = value;
  }
  void connectAndUpload(const sockaddr_in& address, uint32_t key,
                       const DiscoveryCatalog& catalog, uint64_t expected) {
    Socket tcp(::socket(AF_INET, SOCK_STREAM, 0));
    nonblocking(tcp.fd);
    const auto deadline = Clock::now() + std::chrono::milliseconds(config.timeoutMs);
    state("connecting");
    if (::connect(tcp.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
      if (errno != EINPROGRESS) socketError("connect to RecCeiver");
      wait(tcp.fd, POLLOUT, deadline, expected);
      int error = 0; socklen_t length = sizeof(error);
      if (getsockopt(tcp.fd, SOL_SOCKET, SO_ERROR, &error, &length)) socketError("RecCeiver connection status");
      if (error) { errno = error; socketError("connect to RecCeiver"); }
    }
    char host[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    {
      std::lock_guard<std::mutex> guard(mutex);
      current.peer = std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
    }
    Bytes greeting(4, 0), packet;
    append32(greeting, key);
    message(packet, 1, greeting, 64);
    send(tcp.fd, packet, deadline, expected);
    const auto response = receiveMessage(tcp.fd, deadline, expected);
    if (response.first != 0x8001 || response.second.size() != 1 || response.second.front() != 0)
      throw std::runtime_error("unsupported RecCeiver greeting");
    state("uploading");
    send(tcp.fd, catalog.wire, deadline, expected);
    {
      std::lock_guard<std::mutex> guard(mutex);
      current.state = "uploaded";
      ++current.uploads;
    }
    for (;;) {
      auto ping = receiveMessage(tcp.fd,
          Clock::now() + std::chrono::milliseconds(uint64_t(config.timeoutMs) * 4), expected);
      if (ping.first != 0x8002 || ping.second.size() != 4)
        throw std::runtime_error("invalid RecCeiver ping");
      packet.clear(); message(packet, 2, ping.second, 64);
      send(tcp.fd, packet, Clock::now() + std::chrono::milliseconds(config.timeoutMs), expected);
      std::lock_guard<std::mutex> guard(mutex);
      current.state = "synchronized";
      current.synchronizedGeneration = catalog.generation;
      current.lastError.clear();
    }
  }
  void run() noexcept {
    std::mt19937 random(uint32_t(Clock::now().time_since_epoch().count()) ^ uint32_t(getpid()));
    while (!stopping) {
      uint64_t expected = epoch.load();
      try {
        Socket udp(::socket(AF_INET, SOCK_DGRAM, 0));
        nonblocking(udp.fd);
        int reuse = 1;
        if (setsockopt(udp.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) socketError("discovery UDP reuse");
#ifdef SO_REUSEPORT
        if (setsockopt(udp.fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse))) socketError("discovery UDP fanout");
#endif
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(config.udpPort);
        if (inet_pton(AF_INET, config.bindAddress.c_str(), &local.sin_addr) != 1)
          throw std::runtime_error("discovery bind_address must be an IPv4 address");
        if (::bind(udp.fd, reinterpret_cast<sockaddr*>(&local), sizeof(local))) socketError("bind discovery UDP");
        socklen_t size = sizeof(local);
        if (getsockname(udp.fd, reinterpret_cast<sockaddr*>(&local), &size)) socketError("discovery UDP address");
        { std::lock_guard<std::mutex> guard(mutex); current.port = ntohs(local.sin_port); }
        while (!stopping) {
          std::shared_ptr<const DiscoveryCatalog> catalog;
          {
            std::lock_guard<std::mutex> guard(mutex);
            catalog = latest;
            expected = epoch.load();
          }
          try {
            if (!catalog) { wait(-1, 0, Clock::now() + std::chrono::hours(24), expected); continue; }
            state("listening");
            wait(udp.fd, POLLIN, Clock::now() + std::chrono::hours(24), expected);
            sockaddr_in server{};
            uint32_t serverKey = 0;
            bool candidate = false;
            // Announcements may queue while TCP is connected. Consume a bounded
            // batch and use the newest valid endpoint, rather than retrying a
            // departed receiver once per stale datagram after its restart.
            for (unsigned batch = 0; batch < 256; ++batch) {
              check(expected);
              std::array<uint8_t, 64> announcement;
              sockaddr_in peer{}; socklen_t length = sizeof(peer);
              const auto count = recvfrom(udp.fd, announcement.data(), announcement.size(), 0,
                  reinterpret_cast<sockaddr*>(&peer), &length);
              if (count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                socketError("discovery announcement");
              }
              if (count < 16 || read16(announcement.data()) != magic || announcement[2] != 0 ||
                  !read16(announcement.data() + 8) || !read32(announcement.data() + 4)) {
                std::lock_guard<std::mutex> guard(mutex); ++current.invalidAnnouncements; continue;
              }
              server.sin_family = AF_INET;
              server.sin_port = htons(read16(announcement.data() + 8));
              const auto ip = read32(announcement.data() + 4);
              server.sin_addr.s_addr = ip == 0xffffffff ? peer.sin_addr.s_addr : htonl(ip);
              serverKey = read32(announcement.data() + 12);
              candidate = true;
            }
            if (!candidate) continue;
            delay(std::uniform_int_distribution<uint32_t>(0, config.maxHoldoffMs)(random), expected);
            connectAndUpload(server, serverKey, *catalog, expected);
          } catch (const Interrupted&) {
            continue; // Successful reload wakes I/O and replaces only the catalog.
          } catch (const std::exception& error) {
            { std::lock_guard<std::mutex> guard(mutex);
              ++current.failures; current.lastError = std::string(error.what()).substr(0, 1024); current.state = "error"; }
            try { delay(1000, expected); } catch (const Interrupted&) {}
          }
        }
      } catch (const Interrupted&) {
      } catch (const std::exception& error) {
        { std::lock_guard<std::mutex> guard(mutex);
          ++current.failures; current.lastError = std::string(error.what()).substr(0, 1024); current.state = "error"; }
        try { delay(1000, expected); } catch (const Interrupted&) {}
      }
    }
  }
};

DiscoveryPublisher::DiscoveryPublisher(DiscoveryConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}
DiscoveryPublisher::~DiscoveryPublisher() = default;
void DiscoveryPublisher::publish(std::shared_ptr<const DiscoveryCatalog> catalog) {
  if (!catalog) throw std::invalid_argument("discovery catalog must not be null");
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->latest && impl_->latest->generation != impl_->current.synchronizedGeneration) ++impl_->current.coalesced;
    impl_->current.desiredGeneration = catalog->generation;
    impl_->current.records = catalog->records;
    impl_->current.aliases = catalog->aliases;
    impl_->current.bytes = catalog->wire.size();
    impl_->latest = std::move(catalog);
    ++impl_->epoch;
  }
  impl_->wake();
}
DiscoveryStatus DiscoveryPublisher::status() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->current;
}
} // namespace redis_pvxs_ioc
