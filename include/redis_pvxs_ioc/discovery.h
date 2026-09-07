#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct DiscoveryRecord {
  std::string name;
  std::string type;
  std::vector<std::string> aliases;
  std::map<std::string, std::string> properties;
};

// Immutable, prevalidated wire snapshot. Staging has no network effects.
struct DiscoveryCatalog {
  uint64_t generation = 0;
  uint64_t records = 0;
  uint64_t aliases = 0;
  std::vector<uint8_t> wire;
};

std::shared_ptr<const DiscoveryCatalog> prepareDiscoveryCatalog(
    const DiscoveryConfig& limits, uint64_t generation,
    const std::map<std::string, std::string>& identity,
    const std::vector<DiscoveryRecord>& records);

struct DiscoveryStatus {
  std::string state = "disabled";
  std::string peer;
  std::string lastError;
  uint64_t desiredGeneration = 0;
  uint64_t synchronizedGeneration = 0;
  uint64_t records = 0;
  uint64_t aliases = 0;
  uint64_t bytes = 0;
  uint64_t uploads = 0;
  uint64_t failures = 0;
  uint64_t coalesced = 0;
  uint64_t invalidAnnouncements = 0;
  uint16_t port = 0;
};

// One worker, one active catalog and at most one latest replacement. All socket
// I/O is interruptible by stop/reload; no discovery work runs in PVA callbacks.
class DiscoveryPublisher {
public:
  explicit DiscoveryPublisher(DiscoveryConfig config);
  ~DiscoveryPublisher();
  DiscoveryPublisher(const DiscoveryPublisher&) = delete;
  DiscoveryPublisher& operator=(const DiscoveryPublisher&) = delete;
  void publish(std::shared_ptr<const DiscoveryCatalog> catalog);
  DiscoveryStatus status() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redis_pvxs_ioc
