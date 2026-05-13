#pragma once

#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "redis_pvxs_ioc/util.h"

struct redisContext;

namespace redis_pvxs_ioc {

using AlarmStreamFields = std::vector<std::pair<std::string, std::string>>;

AlarmStreamFields makeAlarmStreamFields(const std::string& pvName, const AlarmState& state, std::time_t timestamp);

class AlarmPublisher {
public:
  AlarmPublisher(std::string host, int port, std::string stream, std::string user = {}, std::string password = {});
  ~AlarmPublisher();

  AlarmPublisher(const AlarmPublisher&) = delete;
  AlarmPublisher& operator=(const AlarmPublisher&) = delete;

  void publishTransition(const std::string& pvName, const AlarmState& state);
  bool connected() const;
  const std::string& stream() const;

private:
  bool ensureConnected();
  void resetConnection();

  std::string host_;
  int port_;
  std::string stream_;
  std::string user_;
  std::string password_;
  mutable std::mutex mutex_;
  redisContext* context_ = nullptr;
};

}  // namespace redis_pvxs_ioc
