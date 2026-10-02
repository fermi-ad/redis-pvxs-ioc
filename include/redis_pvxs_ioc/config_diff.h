#pragma once
#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct ConfigDiff {
  std::vector<std::string> added;
  std::vector<std::string> removed;
  std::vector<std::string> replaced;
  std::vector<std::string> metadataChanged;
  std::vector<std::string> accessChanged;
  std::vector<std::string> backendsChanged;
  std::vector<std::string> rpcServicesChanged;
  std::vector<std::string> restartRequired;
  bool alarmStreamChanged = false;
  bool catalogMetadataChanged = false;
};

ConfigDiff diffConfigs(const AppConfig& before, const AppConfig& after);
std::string formatConfigDiff(const ConfigDiff& diff, bool json);
std::string quoteJson(const std::string& value);

} // namespace redis_pvxs_ioc
