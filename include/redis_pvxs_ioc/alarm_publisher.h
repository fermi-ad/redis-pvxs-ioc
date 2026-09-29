#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {

using AlarmStreamFields = std::vector<std::pair<std::string, std::string>>;

AlarmStreamFields makeAlarmStreamFields(const std::string& pvName, const AlarmState& state, std::time_t timestamp);

struct AlarmPublisherOptions {
  size_t stateBytes = 64u * 1024u * 1024u;
  size_t queueEntries = 1024;
  uint32_t ioTimeoutMs = 500, retryMs = 1000, heartbeatMs = 1000;
};
struct AlarmPublisherStatus {
  std::string state = "staged", lastError;
  uint64_t sent = 0, transportFailures = 0, rejected = 0, reconciled = 0;
  uint64_t coalescedUpdates = 0, discardedTransitions = 0, messagesTruncated = 0;
  uint64_t registrations = 0, active = 0, pending = 0, queued = 0;
  uint64_t reservedBytes = 0, peakBytes = 0, byteLimit = 0, queueLimit = 0;
};

class AlarmPublisher {
  struct Impl;
  struct Slot;
public:
  class Registration {
    friend class AlarmPublisher;
    Registration(std::weak_ptr<Impl> owner, std::shared_ptr<Slot> slot);
    std::weak_ptr<Impl> owner_;
    std::shared_ptr<Slot> slot_;
  public:
    ~Registration();
    void update(const AlarmState& state) noexcept;
    void activate() noexcept;
    void retire() noexcept;
  };
  AlarmPublisher(std::string host, int port, std::string stream, std::string user = {},
                 std::string password = {}, AlarmPublisherOptions options = {});
  ~AlarmPublisher();

  AlarmPublisher(const AlarmPublisher&) = delete;
  AlarmPublisher& operator=(const AlarmPublisher&) = delete;

  // Preparation reserves bounded memory but has no Redis effects.
  std::shared_ptr<Registration> prepare(const std::string& name, const AlarmState& state);
  void activate() noexcept;
  void stop();
  bool connected() const;
  const std::string& stream() const;
  AlarmPublisherStatus status() const;

private:
  std::shared_ptr<Impl> impl_;
};

}  // namespace redis_pvxs_ioc
