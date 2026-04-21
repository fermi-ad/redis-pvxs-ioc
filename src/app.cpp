#include "redis_pvxs_ioc/app.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pvxs/nt.h>
#include <pvxs/server.h>

#include "RedisAdapter.hpp"

#include "redis_pvxs_ioc/alarm_publisher.h"
#include "redis_pvxs_ioc/config.h"
#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {
namespace {

pvxs::Value makeAdminValue(const pvxs::TypeCode code, const std::string& description = {}) {
  const bool numeric = code.kind() == pvxs::Kind::Integer || code.kind() == pvxs::Kind::Real;
  auto value = pvxs::nt::NTScalar{code, true, numeric, numeric, numeric}.create();
  if (value["display.description"].valid()) {
    value["display.description"] = description;
  }
  if (value["display.form.choices"].valid()) {
    const auto choices = standardDisplayFormChoices();
    pvxs::shared_array<std::string> array(choices.begin(), choices.end());
    value["display.form.choices"] = array.freeze();
  }
  applyTimestamp(value);
  applyAlarmFields(value, PVConfig{}, AlarmState{0, 0, ""});
  return value;
}

template <typename T>
void setAdminScalar(pvxs::server::SharedPV& pv, const T& input) {
  auto value = pv.fetch();
  assignScalarValue(value, input);
  applyTimestamp(value);
  pv.post(value);
}

class AdminNamespace {
public:
  explicit AdminNamespace(const ServerConfig& serverConfig)
      : reloadCommand_(pvxs::server::SharedPV::buildMailbox()),
        generation_(pvxs::server::SharedPV::buildReadonly()),
        lastStatus_(pvxs::server::SharedPV::buildReadonly()),
        lastError_(pvxs::server::SharedPV::buildReadonly()),
        pvCount_(pvxs::server::SharedPV::buildReadonly()),
        backendHealth_(pvxs::server::SharedPV::buildReadonly()),
        reloadName_(adminPVName(serverConfig, "config:reload")),
        generationName_(adminPVName(serverConfig, "config:generation")),
        lastStatusName_(adminPVName(serverConfig, "config:lastStatus")),
        lastErrorName_(adminPVName(serverConfig, "config:lastError")),
        pvCountName_(adminPVName(serverConfig, "stats:pvCount")),
        backendHealthName_(adminPVName(serverConfig, "backend:health")) {
    auto reloadValue = makeAdminValue(pvxs::TypeCode::Int64, "Write any value to request a config reload");
    reloadValue["value"] = static_cast<int64_t>(0);
    reloadCommand_.onPut([this](pvxs::server::SharedPV& pv,
                                std::unique_ptr<pvxs::server::ExecOp>&& op,
                                pvxs::Value&&) {
      reloadRequested_ = true;
      auto value = pv.fetch();
      value["value"] = value["value"].as<int64_t>() + 1;
      applyTimestamp(value);
      pv.post(value);
      op->reply();
    });
    reloadCommand_.open(reloadValue);

    auto generationValue = makeAdminValue(pvxs::TypeCode::Int64, "Current config generation");
    generationValue["value"] = static_cast<int64_t>(0);
    generation_.open(generationValue);

    auto statusValue = makeAdminValue(pvxs::TypeCode::String, "Last config/app status");
    statusValue["value"] = std::string("idle");
    lastStatus_.open(statusValue);

    auto errorValue = makeAdminValue(pvxs::TypeCode::String, "Last config/app error");
    errorValue["value"] = std::string("");
    lastError_.open(errorValue);

    auto countValue = makeAdminValue(pvxs::TypeCode::Int64, "Configured PV count");
    countValue["value"] = static_cast<int64_t>(0);
    pvCount_.open(countValue);

    auto backendValue = makeAdminValue(pvxs::TypeCode::String, "Redis backend health");
    backendValue["value"] = std::string("unknown");
    backendHealth_.open(backendValue);
  }

  void install(pvxs::server::Server& server) {
    server.addPV(reloadName_, reloadCommand_)
          .addPV(generationName_, generation_)
          .addPV(lastStatusName_, lastStatus_)
          .addPV(lastErrorName_, lastError_)
          .addPV(pvCountName_, pvCount_)
          .addPV(backendHealthName_, backendHealth_);
  }

  void remove(pvxs::server::Server& server) {
    server.removePV(reloadName_)
          .removePV(generationName_)
          .removePV(lastStatusName_)
          .removePV(lastErrorName_)
          .removePV(pvCountName_)
          .removePV(backendHealthName_);
  }

  bool consumeReloadRequest() {
    return reloadRequested_.exchange(false);
  }

  void setGeneration(const uint64_t generation) {
    setAdminScalar(generation_, static_cast<int64_t>(generation));
  }

  void setStatus(const std::string& status) {
    setAdminScalar(lastStatus_, status);
  }

  void setError(const std::string& error) {
    setAdminScalar(lastError_, error);
  }

  void setPvCount(const size_t count) {
    setAdminScalar(pvCount_, static_cast<int64_t>(count));
  }

  void setBackendHealth(const std::string& health) {
    setAdminScalar(backendHealth_, health);
  }

private:
  std::atomic<bool> reloadRequested_{false};
  pvxs::server::SharedPV reloadCommand_;
  pvxs::server::SharedPV generation_;
  pvxs::server::SharedPV lastStatus_;
  pvxs::server::SharedPV lastError_;
  pvxs::server::SharedPV pvCount_;
  pvxs::server::SharedPV backendHealth_;
  std::string reloadName_;
  std::string generationName_;
  std::string lastStatusName_;
  std::string lastErrorName_;
  std::string pvCountName_;
  std::string backendHealthName_;
};

std::shared_ptr<RedisAdapter> buildRedisAdapter(const RedisConfig& config) {
  RA_Options options;
  options.cxn.host = config.host;
  options.cxn.port = config.port;
  if (!config.user.empty()) {
    options.cxn.user = config.user;
  }
  if (!config.password.empty()) {
    options.cxn.password = config.password;
  }
  options.workers = config.workers;
  options.readers = config.readers;
  return std::make_shared<RedisAdapter>(config.baseKey, options);
}

std::shared_ptr<AlarmPublisher> buildAlarmPublisher(const AppConfig& config) {
  return std::make_shared<AlarmPublisher>(config.redis.host, config.redis.port, config.alarms.stream);
}

pvxs::server::Config buildServerConfig(const AppConfig& config) {
  auto serverConfig = pvxs::server::Config::fromEnv();
  if (!config.server.interfaces.empty()) {
    serverConfig.interfaces = config.server.interfaces;
  }
  if (config.server.tcpPort) {
    serverConfig.tcp_port = *config.server.tcpPort;
  }
  if (config.server.udpPort) {
    serverConfig.udp_port = *config.server.udpPort;
  }
  serverConfig.auto_beacon = config.server.autoBeacon;
  return serverConfig;
}

using RuntimeMap = std::unordered_map<std::string, std::shared_ptr<PVRuntimeBase>>;

}  // namespace

struct Application::Impl {
  pvxs::server::Server server;
  std::unique_ptr<AdminNamespace> admin;
  AppConfig currentConfig;
  bool hasConfig = false;
  uint64_t generation = 0;
  std::shared_ptr<RedisAdapter> redis;
  std::shared_ptr<AlarmPublisher> alarmPublisher;
  RuntimeMap runtimes;
  std::chrono::steady_clock::time_point lastHealthUpdate{};
};

Application::Application(std::string configPath)
    : configPath_(std::move(configPath)),
      impl_(std::make_unique<Impl>()) {}

Application::~Application() = default;

bool Application::validateOnly(std::string& summary, std::string& error) const {
  try {
    summary = summarizeConfig(loadConfigFile(configPath_));
    error.clear();
    return true;
  } catch (const std::exception& ex) {
    summary.clear();
    error = ex.what();
    return false;
  }
}

bool Application::start(std::string& error) {
  try {
    const auto config = loadConfigFile(configPath_);
    impl_->server = buildServerConfig(config).build();
    impl_->admin = std::make_unique<AdminNamespace>(config.server);
    impl_->admin->install(impl_->server);
    if (!applyConfig(config, true, error)) {
      return false;
    }
    impl_->server.start();
    started_ = true;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

void Application::requestReload() {
  reloadRequested_.store(true);
}

void Application::pump() {
  if (!started_) {
    return;
  }

  if (impl_->admin && impl_->admin->consumeReloadRequest()) {
    reloadRequested_.store(true);
  }

  if (reloadRequested_.exchange(false)) {
    try {
      auto config = loadConfigFile(configPath_);
      std::string error;
      if (!applyConfig(config, false, error) && impl_->admin) {
        impl_->admin->setStatus("reload rejected");
        impl_->admin->setError(error);
      }
    } catch (const std::exception& ex) {
      if (impl_->admin) {
        impl_->admin->setStatus("reload failed");
        impl_->admin->setError(ex.what());
      }
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - impl_->lastHealthUpdate >= std::chrono::seconds(1)) {
    impl_->lastHealthUpdate = now;
    if (impl_->admin) {
      const auto health = impl_->redis && impl_->redis->connected() ? "connected" : "disconnected";
      impl_->admin->setBackendHealth(health);
    }
  }
}

void Application::stop() {
  if (!started_) {
    return;
  }

  for (auto& item : impl_->runtimes) {
    item.second->deactivate("application stopping");
    impl_->server.removePV(item.first);
  }
  impl_->runtimes.clear();

  if (impl_->admin) {
    impl_->admin->remove(impl_->server);
  }
  impl_->server.stop();
  started_ = false;
}

bool Application::applyConfig(const AppConfig& config, const bool initialLoad, std::string& error) {
  if (!initialLoad && impl_->hasConfig && !sameServerConfig(impl_->currentConfig.server, config.server)) {
    error = "server namespace/bind settings are immutable after startup in the MVP";
    return false;
  }

  const auto nextGeneration = impl_->generation + 1;

  const bool redisChanged = !impl_->hasConfig || !sameRedisConfig(impl_->currentConfig.redis, config.redis);
  const bool alarmChanged = !impl_->hasConfig || !sameAlarmStreamConfig(impl_->currentConfig.alarms, config.alarms);

  if (initialLoad || redisChanged || alarmChanged) {
    return replaceAll(config, nextGeneration, error);
  }
  return applyIncremental(config, nextGeneration, error);
}

bool Application::replaceAll(const AppConfig& config, const uint64_t generation, std::string& error) {
  try {
    auto newRedis = buildRedisAdapter(config.redis);
    newRedis->setDeferReaders(true);
    auto newAlarmPublisher = buildAlarmPublisher(config);

    RuntimeMap staged;
    for (const auto& pv : config.pvs) {
      const auto name = fullPVName(config.server, pv);
      staged.emplace(name, makeRuntime(config.server, pv, newRedis, newAlarmPublisher, generation));
    }
    newRedis->setDeferReaders(false);

    for (auto& item : impl_->runtimes) {
      item.second->deactivate("config replaced");
      impl_->server.removePV(item.first);
    }
    impl_->runtimes.clear();

    for (auto& item : staged) {
      impl_->server.addPV(item.first, item.second->sharedPV());
      impl_->runtimes.emplace(item.first, item.second);
    }

    impl_->redis = std::move(newRedis);
    impl_->alarmPublisher = std::move(newAlarmPublisher);
    impl_->currentConfig = config;
    impl_->hasConfig = true;
    impl_->generation = generation;

    if (impl_->admin) {
      impl_->admin->setGeneration(generation);
      impl_->admin->setPvCount(impl_->runtimes.size());
      impl_->admin->setStatus("generation " + std::to_string(generation) + " active");
      impl_->admin->setError("");
      impl_->admin->setBackendHealth(impl_->redis && impl_->redis->connected() ? "connected" : "disconnected");
    }
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool Application::applyIncremental(const AppConfig& config, const uint64_t generation, std::string& error) {
  try {
    std::map<std::string, PVConfig> desired;
    for (const auto& pv : config.pvs) {
      desired.emplace(fullPVName(config.server, pv), pv);
    }

    std::vector<std::string> removeNames;
    std::vector<std::string> replaceNames;
    std::vector<std::pair<std::string, PVConfig>> addNames;
    std::vector<std::pair<std::string, PVConfig>> reconfigureNames;

    for (const auto& current : impl_->runtimes) {
      const auto desiredIt = desired.find(current.first);
      if (desiredIt == desired.end()) {
        removeNames.push_back(current.first);
        continue;
      }

      if (current.second->structurallyCompatible(desiredIt->second)) {
        reconfigureNames.emplace_back(current.first, desiredIt->second);
      } else {
        replaceNames.push_back(current.first);
      }
    }

    for (const auto& item : desired) {
      if (impl_->runtimes.count(item.first) == 0) {
        addNames.push_back(item);
      }
    }

    RuntimeMap staged;
    if (!replaceNames.empty() || !addNames.empty()) {
      impl_->redis->setDeferReaders(true);
      try {
        for (const auto& name : replaceNames) {
          staged.emplace(name, makeRuntime(config.server, desired.at(name), impl_->redis, impl_->alarmPublisher, generation));
        }
        for (const auto& item : addNames) {
          staged.emplace(item.first, makeRuntime(config.server, item.second, impl_->redis, impl_->alarmPublisher, generation));
        }
      } catch (...) {
        impl_->redis->setDeferReaders(false);
        for (auto& item : staged) {
          item.second->deactivate("staged config failure");
        }
        throw;
      }
      impl_->redis->setDeferReaders(false);
    }

    for (const auto& item : reconfigureNames) {
      impl_->runtimes.at(item.first)->reconfigure(item.second, generation);
    }

    for (const auto& name : removeNames) {
      impl_->runtimes.at(name)->deactivate("pv removed");
      impl_->server.removePV(name);
      impl_->runtimes.erase(name);
    }

    for (const auto& name : replaceNames) {
      impl_->runtimes.at(name)->deactivate("pv replaced");
      impl_->server.removePV(name);
      impl_->runtimes.erase(name);
      impl_->server.addPV(name, staged.at(name)->sharedPV());
      impl_->runtimes.emplace(name, staged.at(name));
      staged.erase(name);
    }

    for (auto& item : staged) {
      impl_->server.addPV(item.first, item.second->sharedPV());
      impl_->runtimes.emplace(item.first, item.second);
    }

    impl_->currentConfig = config;
    impl_->generation = generation;

    if (impl_->admin) {
      impl_->admin->setGeneration(generation);
      impl_->admin->setPvCount(impl_->runtimes.size());
      impl_->admin->setStatus("generation " + std::to_string(generation) + " active");
      impl_->admin->setError("");
      impl_->admin->setBackendHealth(impl_->redis && impl_->redis->connected() ? "connected" : "disconnected");
    }
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

}  // namespace redis_pvxs_ioc
