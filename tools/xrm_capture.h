#pragma once

#include "xrm_demux.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace xrm_capture {

inline constexpr size_t pulseBytes = 524288;
inline constexpr size_t samplesPerChannel = 16383;

struct Config {
  std::string host;
  unsigned port = 4210;
  unsigned initialReconnectMs = 1000, maxReconnectMs = 30000;
  size_t queueLimitBytes = 32u * 1024u * 1024u;
  xrm_demux::Config redis;
  xrm_demux::Source source;
};

// These functions only validate local configuration; no Redis or device I/O.
Config parseConfig(const std::string& yaml, const xrm_demux::Config&);
Config loadConfig(const std::string& path, const xrm_demux::Config&);

class FramingError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One connection generation at a time. Only validated, complete original
// pulses reach the sink. An invalid search spanning two pulse lengths latches
// a fault, rather than allocating or searching indefinitely.
class Framer {
 public:
  using Sink = std::function<void(std::string&&)>;
  void feed(const char* bytes, size_t count, const Sink&);
  void reset();
  uint64_t rejected() const { return rejected_; }
  uint64_t discardedBytes() const { return discarded_; }
  size_t bufferedBytes() const { return buffer_.size(); }
  bool faulted() const { return faulted_; }
 private:
  void process(const Sink&);
  void discard(size_t count);
  std::string buffer_;
  size_t searching_ = 0;
  uint64_t rejected_ = 0, discarded_ = 0;
  bool faulted_ = false;
};

struct Status {
  bool running = false;
  std::string state = "stopped", leaseState = "unknown", error;
  uint64_t received = 0, published = 0, reconnects = 0;
  uint64_t localDrops = 0, rejects = 0, discardedBytes = 0;
  uint64_t redisFailures = 0, uncertainWrites = 0, logDrops = 0;
  size_t queueDepth = 0, queueHighWater = 0, queueBytes = 0;
  int64_t lastReceiveAgeMs = -1, lastPublishAgeMs = -1;
};

class Worker {
 public:
  explicit Worker(Config);
  ~Worker();
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  void start();
  void stop() noexcept;
  Status status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xrm_capture
