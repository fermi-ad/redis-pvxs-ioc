#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>

#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/config.h"

class RedisAdapter;

namespace redis_pvxs_ioc {

class AlarmPublisher;
class OperationQueue;

using RedisBackendRegistry = std::map<std::string, std::shared_ptr<RedisAdapter>>;

struct RuntimeStats {
  std::atomic<uint64_t> ndarrayInvalidFrames{0};
  std::atomic<uint64_t> ndarraySkippedFrames{0};
  std::atomic<uint64_t> ndarrayDiscontinuities{0};
};

class PVRuntimeUpdate {
public:
  virtual ~PVRuntimeUpdate() = default;
  virtual void commit() noexcept = 0;
  virtual void refresh() = 0;
};

class PVRuntimeBase {
public:
  virtual ~PVRuntimeBase() = default;

  virtual const PVConfig& config() const = 0;
  virtual const std::string& fullName() const = 0;
  virtual pvxs::server::SharedPV& sharedPV() = 0;
  virtual bool structurallyCompatible(const PVConfig& config) const = 0;
  virtual void reconfigure(const PVConfig& config, uint64_t generation) = 0;
  virtual std::unique_ptr<PVRuntimeUpdate> prepareReconfigure(const PVConfig& config, uint64_t generation,
      const std::shared_ptr<AlarmPublisher>& publisher = {}) = 0;
  virtual void activate() noexcept = 0;
  virtual void deactivate(const std::string& reason) = 0;
};

std::shared_ptr<PVRuntimeBase> makeRuntime(const ServerConfig& serverConfig,
                                           const PVConfig& config,
                                           const RedisBackendRegistry& redisBackends,
                                           const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                           uint64_t generation,
                                           std::shared_ptr<OperationQueue> operations = {},
                                           OperationLimitsConfig limits = {},
                                           std::shared_ptr<RuntimeStats> stats = {});

}  // namespace redis_pvxs_ioc
