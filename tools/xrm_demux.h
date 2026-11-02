#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xrm_demux {

struct Channel {
  unsigned channel = 0;
  std::string raw, volts;
  float eslo = 1.0f, eoff = 0.0f;
};
struct Source {
  std::string stream, checkpoint, payloadField = "_", startId;
  std::string timestampMode, timestampField, timestampValidField;
  size_t samplesPerChannel = 0, retention = 0;
  std::vector<Channel> channels;
  std::string fingerprint;
};
struct Config {
  std::string host = "127.0.0.1";
  unsigned port = 6379, db = 0;
  unsigned connectTimeoutMs = 200, commandTimeoutMs = 500;
  unsigned pollIntervalMs = 100, retryMs = 1000;
  std::vector<Source> sources;
};
struct Status {
  bool running = false;
  std::string state = "stopped", error;
  uint64_t processed = 0, rejected = 0, failures = 0, gaps = 0;
};
struct Record {
  std::string key;
  std::map<std::string, std::string> fields;
};

// Parsing and decoding have no Redis or hardware side effects.
Config parseConfig(const std::string& yaml);
Config loadConfig(const std::string& path);
std::vector<Record> decode(const Source&, const std::string& sourceId,
                           const std::string& payload, const std::string& timestamp,
                           const std::string& timestampValid, bool continuityUnknown = false);

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

} // namespace xrm_demux
