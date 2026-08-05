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

using RedisBackendRegistry = std::map<std::string, std::shared_ptr<RedisAdapter>>;

struct RuntimeStats {
  std::atomic<uint64_t> ndarrayInvalidFrames{0};
  std::atomic<uint64_t> ndarraySkippedFrames{0};
};

class PVRuntimeBase {
public:
  virtual ~PVRuntimeBase() = default;

  virtual const PVConfig& config() const = 0;
  virtual const std::string& fullName() const = 0;
  virtual pvxs::server::SharedPV& sharedPV() = 0;
  virtual bool structurallyCompatible(const PVConfig& config) const = 0;
  virtual void reconfigure(const PVConfig& config, uint64_t generation) = 0;
  virtual void deactivate(const std::string& reason) = 0;
};

std::shared_ptr<PVRuntimeBase> makeRuntime(const ServerConfig& serverConfig,
                                           const PVConfig& config,
                                           const RedisBackendRegistry& redisBackends,
                                           const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                           const std::shared_ptr<RuntimeStats>& stats,
                                           uint64_t generation);

}  // namespace redis_pvxs_ioc
