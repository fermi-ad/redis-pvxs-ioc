#include "redis_pvxs_ioc/app.h"
#include "redis_pvxs_ioc/endpoints.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>
#include <cstdlib>
#include <epicsVersion.h>

#include <pvxs/nt.h>
#include <pvxs/server.h>

#include "RedisAdapter.hpp"

#include "redis_pvxs_ioc/alarm_publisher.h"
#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/config.h"
#include "redis_pvxs_ioc/config_diff.h"
#include "redis_pvxs_ioc/discovery.h"
#if REDIS_PVXS_IOC_ENABLE_GRPC
#include "redis_pvxs_ioc/rpc_pv.h"
#endif
#include "redis_pvxs_ioc/runtime.h"
#include "redis_pvxs_ioc/util.h"
#include "redis_pvxs_ioc/version.h"

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

void openStringPV(pvxs::server::SharedPV& pv,
                  const std::string& scalar,
                  const std::string& description) {
  auto value = makeAdminValue(pvxs::TypeCode::String, description);
  value["value"] = scalar;
  pv.open(value);
}

class AdminNamespace {
public:
  explicit AdminNamespace(const ServerConfig& serverConfig, const bool accessConfigured)
      : serverConfig_(serverConfig), accessConfigured_(accessConfigured),
        reloadCommand_(pvxs::server::SharedPV::buildMailbox()),
        accessReloadCommand_(pvxs::server::SharedPV::buildMailbox()),
        version_(pvxs::server::SharedPV::buildReadonly()),
        revision_(pvxs::server::SharedPV::buildReadonly()),
        sysVersion_(pvxs::server::SharedPV::buildReadonly()),
        sysRevision_(pvxs::server::SharedPV::buildReadonly()),
        generation_(pvxs::server::SharedPV::buildReadonly()),
        lastStatus_(pvxs::server::SharedPV::buildReadonly()),
        lastError_(pvxs::server::SharedPV::buildReadonly()),
        pvCount_(pvxs::server::SharedPV::buildReadonly()),
        backendHealth_(pvxs::server::SharedPV::buildReadonly()),
        accessEnabled_(pvxs::server::SharedPV::buildReadonly()),
        accessGeneration_(pvxs::server::SharedPV::buildReadonly()),
        accessLastStatus_(pvxs::server::SharedPV::buildReadonly()),
        accessLastError_(pvxs::server::SharedPV::buildReadonly()),
        accessPolicyFingerprint_(pvxs::server::SharedPV::buildReadonly()),
        accessWatchStatus_(pvxs::server::SharedPV::buildReadonly()),
        accessActiveClients_(pvxs::server::SharedPV::buildReadonly()),
        accessDeniedReads_(pvxs::server::SharedPV::buildReadonly()),
        accessDeniedWrites_(pvxs::server::SharedPV::buildReadonly()),
        accessRightsChanges_(pvxs::server::SharedPV::buildReadonly()),
        reloadName_(adminPVName(serverConfig, "config:reload")),
        versionName_(versionPVName(serverConfig)),
        revisionName_(revisionPVName(serverConfig)),
        sysVersionName_(adminPVName(serverConfig, "version")),
        sysRevisionName_(adminPVName(serverConfig, "revision")),
        generationName_(adminPVName(serverConfig, "config:generation")),
        lastStatusName_(adminPVName(serverConfig, "config:lastStatus")),
        lastErrorName_(adminPVName(serverConfig, "config:lastError")),
        pvCountName_(adminPVName(serverConfig, "stats:pvCount")),
        backendHealthName_(adminPVName(serverConfig, "backend:health")) {
    lastDiffName_ = adminPVName(serverConfig, "config:lastDiff");
    openStringPV(lastDiff_, "{}", "Last parsed reload differences; credential values omitted");
    discoveryName_ = adminPVName(serverConfig, "discovery:status");
    using pvxs::TypeCode;
    using pvxs::Member;
    auto discoveryValue = pvxs::TypeDef(TypeCode::Struct, "redis-pvxs-ioc:discovery:1.0", {
      Member(TypeCode::String, "state"), Member(TypeCode::String, "peer"), Member(TypeCode::String, "lastError"),
      Member(TypeCode::UInt64, "desiredGeneration"), Member(TypeCode::UInt64, "synchronizedGeneration"),
      Member(TypeCode::UInt64, "records"), Member(TypeCode::UInt64, "aliases"), Member(TypeCode::UInt64, "bytes"),
      Member(TypeCode::UInt64, "uploads"), Member(TypeCode::UInt64, "failures"), Member(TypeCode::UInt64, "coalesced"),
      Member(TypeCode::UInt64, "invalidAnnouncements"), Member(TypeCode::UInt16, "port")}).create();
    discoveryValue["state"] = "disabled";
    discovery_.open(discoveryValue);
    accessReloadName_ = adminPVName(serverConfig, "access:reload");
    accessEnabledName_ = adminPVName(serverConfig, "access:enabled");
    accessGenerationName_ = adminPVName(serverConfig, "access:generation");
    accessLastStatusName_ = adminPVName(serverConfig, "access:lastStatus");
    accessLastErrorName_ = adminPVName(serverConfig, "access:lastError");
    accessPolicyFingerprintName_ = adminPVName(serverConfig, "access:policyFingerprint");
    accessWatchStatusName_ = adminPVName(serverConfig, "access:watchStatus");
    accessActiveClientsName_ = adminPVName(serverConfig, "access:activeClients");
    accessDeniedReadsName_ = adminPVName(serverConfig, "access:deniedReads");
    accessDeniedWritesName_ = adminPVName(serverConfig, "access:deniedWrites");
    accessRightsChangesName_ = adminPVName(serverConfig, "access:rightsChanges");
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

    auto accessReloadValue = makeAdminValue(pvxs::TypeCode::Int64, "Write any value to request an ACF reload");
    accessReloadValue["value"] = static_cast<int64_t>(0);
    accessReloadCommand_.onPut([this](pvxs::server::SharedPV& pv,
                                      std::unique_ptr<pvxs::server::ExecOp>&& op,
                                      pvxs::Value&&) {
      if (!accessConfigured_) {
        op->error("access control is disabled");
        return;
      }
      accessReloadRequested_ = true;
      auto value = pv.fetch();
      value["value"] = value["value"].as<int64_t>() + 1;
      applyTimestamp(value);
      pv.post(value);
      op->reply();
    });
    accessReloadCommand_.open(accessReloadValue);

    const std::string version = std::string("redis-pvxs-ioc v") + REDIS_PVXS_IOC_VERSION;
    const std::string revision = std::string("redis-pvxs-ioc ") + REDIS_PVXS_IOC_GIT_REVISION;
    openStringPV(version_, version, "redis-pvxs-ioc version");
    openStringPV(sysVersion_, version, "redis-pvxs-ioc version");
    openStringPV(revision_, revision, "redis-pvxs-ioc git revision");
    openStringPV(sysRevision_, revision, "redis-pvxs-ioc git revision");

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

    auto enabledValue = makeAdminValue(pvxs::TypeCode::Bool, "Whether ACF access control was enabled at startup");
    enabledValue["value"] = false;
    accessEnabled_.open(enabledValue);
    auto accessGenerationValue = makeAdminValue(pvxs::TypeCode::Int64, "Current access policy generation");
    accessGenerationValue["value"] = static_cast<int64_t>(0);
    accessGeneration_.open(accessGenerationValue);
    openStringPV(accessLastStatus_, "disabled", "Last access policy status");
    openStringPV(accessLastError_, "", "Last access policy error");
    openStringPV(accessPolicyFingerprint_, "", "Expanded access policy fingerprint");
    openStringPV(accessWatchStatus_, "disabled", "Access policy file watcher status");
    auto activeValue = makeAdminValue(pvxs::TypeCode::Int64, "Connected access-controlled channels");
    activeValue["value"] = static_cast<int64_t>(0);
    accessActiveClients_.open(activeValue);
    auto deniedReadValue = makeAdminValue(pvxs::TypeCode::Int64, "Denied read operations since startup");
    deniedReadValue["value"] = static_cast<int64_t>(0);
    accessDeniedReads_.open(deniedReadValue);
    auto deniedWriteValue = makeAdminValue(pvxs::TypeCode::Int64, "Denied write operations since startup");
    deniedWriteValue["value"] = static_cast<int64_t>(0);
    accessDeniedWrites_.open(deniedWriteValue);
    auto rightsValue = makeAdminValue(pvxs::TypeCode::Int64, "Access-right changes since startup");
    rightsValue["value"] = static_cast<int64_t>(0);
    accessRightsChanges_.open(rightsValue);
  }

  void setLastDiff(const std::string& diff) { setAdminScalar(lastDiff_, diff); }

  void setDiscoveryStatus(const DiscoveryStatus& status) {
    auto value = discovery_.fetch();
    value["state"] = status.state; value["peer"] = status.peer; value["lastError"] = status.lastError;
    value["desiredGeneration"] = status.desiredGeneration; value["synchronizedGeneration"] = status.synchronizedGeneration;
    value["records"] = status.records; value["aliases"] = status.aliases; value["bytes"] = status.bytes;
    value["uploads"] = status.uploads; value["failures"] = status.failures; value["coalesced"] = status.coalesced;
    value["invalidAnnouncements"] = status.invalidAnnouncements; value["port"] = status.port;
    discovery_.post(value);
  }

  void install(pvxs::server::Server& server,
               AccessController* access,
               const AccessDefaultsConfig& defaults) {
    std::map<std::string, std::pair<pvxs::server::SharedPV, AccessAssignment>> bindings;
    EndpointRegistry endpoints;
    const auto add = [&](const std::string& name, const pvxs::server::SharedPV& pv,
                         const AccessAssignment& assignment) {
      endpoints.reserve(name, EndpointKind::Diagnostic, "runtime");
      bindings.emplace(name, std::make_pair(pv, assignment));
    };
    add(lastDiffName_, lastDiff_, defaults.adminRead);
    add(discoveryName_, discovery_, defaults.adminRead);
    add(reloadName_, reloadCommand_, defaults.adminWrite);
    add(versionName_, version_, defaults.adminRead);
    add(revisionName_, revision_, defaults.adminRead);
    add(sysVersionName_, sysVersion_, defaults.adminRead);
    add(sysRevisionName_, sysRevision_, defaults.adminRead);
    add(generationName_, generation_, defaults.adminRead);
    add(lastStatusName_, lastStatus_, defaults.adminRead);
    add(lastErrorName_, lastError_, defaults.adminRead);
    add(pvCountName_, pvCount_, defaults.adminRead);
    add(backendHealthName_, backendHealth_, defaults.adminRead);
    add(accessReloadName_, accessReloadCommand_, defaults.adminWrite);
    add(accessEnabledName_, accessEnabled_, defaults.adminRead);
    add(accessGenerationName_, accessGeneration_, defaults.adminRead);
    add(accessLastStatusName_, accessLastStatus_, defaults.adminRead);
    add(accessLastErrorName_, accessLastError_, defaults.adminRead);
    add(accessPolicyFingerprintName_, accessPolicyFingerprint_, defaults.adminRead);
    add(accessWatchStatusName_, accessWatchStatus_, defaults.adminRead);
    add(accessActiveClientsName_, accessActiveClients_, defaults.adminRead);
    add(accessDeniedReadsName_, accessDeniedReads_, defaults.adminRead);
    add(accessDeniedWritesName_, accessDeniedWrites_, defaults.adminRead);
    add(accessRightsChangesName_, accessRightsChanges_, defaults.adminRead);
    const auto reserved = adminPVNames(serverConfig_);
    if (bindings.size() != reserved.size())
      throw std::logic_error("installed diagnostics do not match the reserved namespace");
    for (const auto& name : reserved)
      if (!bindings.count(name))
        throw std::logic_error("reserved diagnostic is not installed: " + name);
    for (const auto& binding : bindings) {
      if (access) access->addPV(binding.first, binding.second.first, binding.second.second);
      else server.addPV(binding.first, binding.second.first);
    }
  }

  void remove(pvxs::server::Server& server, AccessController* access) {
    const auto remove = [&](const std::string& name) {
      if (access) access->removePV(name);
      else server.removePV(name);
    };
    remove(lastDiffName_);
    remove(discoveryName_);
    remove(reloadName_);
    remove(versionName_);
    remove(revisionName_);
    remove(sysVersionName_);
    remove(sysRevisionName_);
    remove(generationName_);
    remove(lastStatusName_);
    remove(lastErrorName_);
    remove(pvCountName_);
    remove(backendHealthName_);
    remove(accessReloadName_);
    remove(accessEnabledName_);
    remove(accessGenerationName_);
    remove(accessLastStatusName_);
    remove(accessLastErrorName_);
    remove(accessPolicyFingerprintName_);
    remove(accessWatchStatusName_);
    remove(accessActiveClientsName_);
    remove(accessDeniedReadsName_);
    remove(accessDeniedWritesName_);
    remove(accessRightsChangesName_);
  }

  void setAccessAssignments(AccessController& access, const AccessDefaultsConfig& defaults) {
    access.setAssignment(reloadName_, defaults.adminWrite);
    access.setAssignment(accessReloadName_, defaults.adminWrite);
    const std::vector<std::string> readNames{
      lastDiffName_, discoveryName_, versionName_, revisionName_, sysVersionName_, sysRevisionName_, generationName_,
      lastStatusName_, lastErrorName_, pvCountName_, backendHealthName_, accessEnabledName_,
      accessGenerationName_, accessLastStatusName_, accessLastErrorName_,
      accessPolicyFingerprintName_, accessWatchStatusName_, accessActiveClientsName_,
      accessDeniedReadsName_, accessDeniedWritesName_, accessRightsChangesName_,
    };
    for (const auto& name : readNames) access.setAssignment(name, defaults.adminRead);
  }

  bool consumeReloadRequest() {
    return reloadRequested_.exchange(false);
  }

  bool consumeAccessReloadRequest() {
    return accessReloadRequested_.exchange(false);
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

  void setAccessStatus(const AccessStatus& status) {
    setAdminScalar(accessEnabled_, status.enabled);
    setAdminScalar(accessGeneration_, static_cast<int64_t>(status.generation));
    setAdminScalar(accessLastStatus_, status.lastStatus);
    setAdminScalar(accessLastError_, status.lastError);
    setAdminScalar(accessPolicyFingerprint_, status.policyFingerprint);
    setAdminScalar(accessWatchStatus_, status.watchStatus);
    setAdminScalar(accessActiveClients_, static_cast<int64_t>(status.activeClients));
    setAdminScalar(accessDeniedReads_, static_cast<int64_t>(status.deniedReads));
    setAdminScalar(accessDeniedWrites_, static_cast<int64_t>(status.deniedWrites));
    setAdminScalar(accessRightsChanges_, static_cast<int64_t>(status.rightsChanges));
  }

private:
  const ServerConfig serverConfig_;
  bool accessConfigured_ = false;
  std::atomic<bool> reloadRequested_{false};
  std::atomic<bool> accessReloadRequested_{false};
  pvxs::server::SharedPV lastDiff_ = pvxs::server::SharedPV::buildReadonly();
  std::string lastDiffName_;
  pvxs::server::SharedPV discovery_ = pvxs::server::SharedPV::buildReadonly();
  std::string discoveryName_;
  pvxs::server::SharedPV reloadCommand_;
  pvxs::server::SharedPV accessReloadCommand_;
  pvxs::server::SharedPV version_;
  pvxs::server::SharedPV revision_;
  pvxs::server::SharedPV sysVersion_;
  pvxs::server::SharedPV sysRevision_;
  pvxs::server::SharedPV generation_;
  pvxs::server::SharedPV lastStatus_;
  pvxs::server::SharedPV lastError_;
  pvxs::server::SharedPV pvCount_;
  pvxs::server::SharedPV backendHealth_;
  pvxs::server::SharedPV accessEnabled_;
  pvxs::server::SharedPV accessGeneration_;
  pvxs::server::SharedPV accessLastStatus_;
  pvxs::server::SharedPV accessLastError_;
  pvxs::server::SharedPV accessPolicyFingerprint_;
  pvxs::server::SharedPV accessWatchStatus_;
  pvxs::server::SharedPV accessActiveClients_;
  pvxs::server::SharedPV accessDeniedReads_;
  pvxs::server::SharedPV accessDeniedWrites_;
  pvxs::server::SharedPV accessRightsChanges_;
  std::string reloadName_;
  std::string versionName_;
  std::string revisionName_;
  std::string sysVersionName_;
  std::string sysRevisionName_;
  std::string generationName_;
  std::string lastStatusName_;
  std::string lastErrorName_;
  std::string pvCountName_;
  std::string backendHealthName_;
  std::string accessReloadName_;
  std::string accessEnabledName_;
  std::string accessGenerationName_;
  std::string accessLastStatusName_;
  std::string accessLastErrorName_;
  std::string accessPolicyFingerprintName_;
  std::string accessWatchStatusName_;
  std::string accessActiveClientsName_;
  std::string accessDeniedReadsName_;
  std::string accessDeniedWritesName_;
  std::string accessRightsChangesName_;
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

RedisBackendRegistry buildRedisBackends(const AppConfig& config,
                                       const AppConfig& previous,
                                       const RedisBackendRegistry& existing) {
  RedisBackendRegistry backends;
  for (const auto& entry : config.redisBackends) {
    const auto old = previous.redisBackends.find(entry.first);
    const auto runtime = existing.find(entry.first);
    if (old != previous.redisBackends.end() && runtime != existing.end() &&
        sameRedisConfig(entry.second, old->second))
      backends.emplace(entry.first, runtime->second);
    else
      backends.emplace(entry.first, buildRedisAdapter(entry.second));
  }
  return backends;
}

bool sameBackendBindings(const PVConfig& pv, const RedisBackendRegistry& before,
                         const RedisBackendRegistry& after) {
  const auto same = [&](const std::string& name) {
    const auto a = before.find(name), b = after.find(name);
    return a != before.end() && b != after.end() && a->second == b->second;
  };
  return same(pv.read.backend) && (!pv.write || same(pv.write->backend)) &&
         (!pv.confirm || same(pv.confirm->backend));
}

std::shared_ptr<AlarmPublisher> buildAlarmPublisher(const AppConfig& config) {
  const auto backend = config.redisBackends.at(config.alarms.backend);
  return std::make_shared<AlarmPublisher>(backend.host, backend.port, config.alarms.stream, backend.user, backend.password);
}

std::string backendHealthSummary(const RedisBackendRegistry& backends) {
  if (backends.empty()) {
    return "0/0 connected";
  }

  size_t connected = 0;
  std::vector<std::string> disconnected;
  for (const auto& entry : backends) {
    if (entry.second && entry.second->connected()) {
      ++connected;
      continue;
    }
    disconnected.push_back(entry.first);
  }

  std::ostringstream stream;
  stream << connected << "/" << backends.size() << " connected";
  if (!disconnected.empty()) {
    stream << " (";
    for (size_t index = 0; index < disconnected.size(); ++index) {
      if (index != 0u) {
        stream << ",";
      }
      stream << disconnected[index];
    }
    stream << " disconnected)";
  }
  return stream.str();
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
using RpcMap = std::unordered_map<std::string, std::shared_ptr<pvxs::server::SharedPV>>;
using PVBindingMap = std::map<std::string, std::string>;
using AssignmentMap = std::unordered_map<std::string, AccessAssignment>;
struct RpcServiceState {
  RpcServiceConfig config;
  RpcMap pvs;
  std::map<std::string, std::string> owners;
};
using RpcServices = std::vector<RpcServiceState>;

PVBindingMap pvBindings(const AppConfig& config) {
  PVBindingMap bindings;
  for (const auto& pv : config.pvs) {
    const auto canonicalName = fullPVName(config.server, pv);
    for (const auto& servedName : fullPVNames(config.server, pv)) {
      bindings.emplace(servedName, canonicalName);
    }
  }
  return bindings;
}

std::set<std::string> requiredAccessAsgs(const AppConfig& config) {
  std::set<std::string> groups;
  if (!config.access.enabled) return groups;
  groups.insert(config.access.defaults.adminRead.asg);
  groups.insert(config.access.defaults.adminWrite.asg);
  for (const auto& pv : config.pvs) groups.insert(pv.access.value_or(config.access.defaults.pv).asg);
  for (const auto& service : config.rpcServices) {
    groups.insert(service.access.value_or(config.access.defaults.rpc).asg);
  }
  return groups;
}

// Build RPC-forwarding PVs by reflecting each configured gRPC service and
// creating one PV per method, named <namespace>:<UPPER_SNAKE(Method)><suffix>.
// The IOC has no compiled-in knowledge of the methods or message schema.
RpcMap buildRpcPVs(const AppConfig& config, AssignmentMap& assignments,
                  const RpcServices& previous, RpcServices& staged) {
  RpcMap rpcPVs;
  auto endpoints = configuredEndpoints(config);
  assignments.clear();
  staged.clear();
#if REDIS_PVXS_IOC_ENABLE_GRPC
  for (const auto& svc : config.rpcServices) {
    const auto prior = std::find_if(previous.begin(), previous.end(), [&](const auto& entry) {
      return !entry.pvs.empty() && entry.config.endpoint == svc.endpoint &&
             entry.config.service == svc.service && entry.config.suffix == svc.suffix &&
             entry.config.defaults == svc.defaults;
    });
    if (prior != previous.end()) {
      for (const auto& item : prior->pvs) {
        endpoints.reserve(item.first, EndpointKind::RPC, prior->owners.at(item.first), "root.rpc_services");
        rpcPVs.emplace(item);
        assignments.emplace(item.first, svc.access.value_or(config.access.defaults.rpc));
      }
      staged.push_back(*prior);
      staged.back().config = svc;
      continue;
    }
    auto bridge = std::make_shared<GrpcBridge>(svc.endpoint);

    // The backend may not be up yet at IOC startup; retry reflection briefly.
    std::vector<BridgeMethod> methods;
    std::string lastErr;
    for (int attempt = 0; attempt < 30; ++attempt) {
      try {
        methods = bridge->discover(svc.service);
        break;
      } catch (const std::exception& e) {
        lastErr = e.what();
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
    if (methods.empty()) {
      std::fprintf(stderr,
                   "[redis-pvxs-ioc] rpc_service %s @ %s: reflection failed (%s); "
                   "no RPC PVs created for it\n",
                   svc.service.c_str(), svc.endpoint.c_str(), lastErr.c_str());
      continue;
    }

    RpcServiceState serviceState;
    serviceState.config = svc;
    for (const auto& m : methods) {
      std::string leaf = methodToPvLeaf(m.method) + svc.suffix;
      std::string name =
          config.server.nameSpace.empty() ? leaf : config.server.nameSpace + ":" + leaf;
      endpoints.reserve(name, EndpointKind::RPC, m.service + "/" + m.method, "root.rpc_services");
      auto runtime = std::make_shared<RpcPV>(bridge, m, svc.defaults);
      auto pv = std::shared_ptr<pvxs::server::SharedPV>(runtime, &runtime->sharedPV());
      rpcPVs.emplace(name, pv);
      serviceState.pvs.emplace(name, std::move(pv));
      serviceState.owners.emplace(name, m.service + "/" + m.method);
      assignments.emplace(name, svc.access.value_or(config.access.defaults.rpc));
      std::fprintf(stderr, "[redis-pvxs-ioc] rpc PV %s -> %s/%s\n",
                   name.c_str(), m.service.c_str(), m.method.c_str());
    }
    staged.push_back(std::move(serviceState));
  }
#else
  if (!config.rpcServices.empty())
    throw std::runtime_error("rpc_services requires a build with REDIS_PVXS_IOC_ENABLE_GRPC=ON");
#endif
  return rpcPVs;
}

std::shared_ptr<const DiscoveryCatalog> buildDiscoveryCatalog(
    const AppConfig& config, const RpcMap& rpcs, unsigned short port, uint64_t generation) {
  if (!config.discovery.enabled) return {};
  std::map<std::string, std::string> identity{
    {"IOCNAME", config.server.instance}, {"PVAS_SERVER_PORT", std::to_string(port)},
    {"PVXS_PROTOCOL", "pva"}, {"EPICS_VERSION", EPICS_VERSION_STRING},
    {"REDIS_PVXS_IOC_VERSION", REDIS_PVXS_IOC_VERSION}, {"CONFIG_GENERATION", std::to_string(generation)}};
  char hostname[256]{};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0) identity["HOSTNAME"] = hostname;
  for (const auto* key : {"HOSTNAME", "ENGINEER", "LOCATION", "CONTACT", "BUILDING", "SECTOR"})
    if (const auto* value = std::getenv(key); value && *value) identity[key] = value;
  std::vector<DiscoveryRecord> records;
  for (const auto& pv : config.pvs) {
    DiscoveryRecord record;
    record.name = fullPVName(config.server, pv);
    record.type = pv.shape == Shape::Array ? "epics:nt/NTScalarArray:1.0" : "epics:nt/NTScalar:1.0";
    record.aliases = pv.aliases;
    record.properties = {{"protocol", "pva"}, {"DESC", pv.metadata.description}, {"units", pv.metadata.units},
                         {"type", toString(pv.type)}, {"shape", toString(pv.shape)}};
    records.push_back(std::move(record));
  }
  for (const auto& name : adminPVNames(config.server))
    records.push_back({name, "pvxs:diagnostic", {}, {{"protocol", "pva"}}});
  for (const auto& rpc : rpcs) records.push_back({rpc.first, "pvxs:RPC", {}, {{"protocol", "pva"}}});
  return prepareDiscoveryCatalog(config.discovery, generation, identity, records);
}

}  // namespace

struct Application::Impl {
  pvxs::server::Server server;
  std::unique_ptr<DiscoveryPublisher> discovery;
  std::shared_ptr<const DiscoveryCatalog> catalog;
  std::shared_ptr<AccessController> access;
  std::unique_ptr<AdminNamespace> admin;
  AppConfig currentConfig;
  bool hasConfig = false;
  uint64_t generation = 0;
  RedisBackendRegistry redisBackends;
  std::shared_ptr<AlarmPublisher> alarmPublisher;
  RuntimeMap runtimes;
  RpcMap rpcPVs;
  AssignmentMap rpcAssignments;
  RpcServices rpcServices;
  std::chrono::steady_clock::time_point lastHealthUpdate{};

  void addEndpoint(const std::string& name,
                   const pvxs::server::SharedPV& pv,
                   const AccessAssignment& assignment) {
    if (access) access->addPV(name, pv, assignment);
    else server.addPV(name, pv);
  }

  void removeEndpoint(const std::string& name) {
    if (access) access->removePV(name);
    else server.removePV(name);
  }

  void setAssignment(const std::string& name, const AccessAssignment& assignment) {
    if (access) access->setAssignment(name, assignment);
  }
};

Application::Application(std::string configPath)
    : configPath_(std::move(configPath)),
      impl_(std::make_unique<Impl>()) {}

Application::~Application() = default;

bool Application::validateOnly(std::string& summary, std::string& error, AppConfig* normalized) const {
  try {
    const auto config = loadConfigFile(configPath_);
#if !REDIS_PVXS_IOC_ENABLE_GRPC
    if (!config.rpcServices.empty())
      throw std::runtime_error("rpc_services requires a build with REDIS_PVXS_IOC_ENABLE_GRPC=ON");
#endif
    std::string policyFingerprint;
    buildDiscoveryCatalog(config, {}, config.server.tcpPort.value_or(5075), 1);
    if (!validateAccessPolicy(config.access, requiredAccessAsgs(config), policyFingerprint, error)) {
      summary.clear();
      return false;
    }
    summary = summarizeConfig(config);
    if (normalized) *normalized = config;
    if (!policyFingerprint.empty()) summary += " access_fingerprint=" + policyFingerprint;
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
    if (config.access.enabled) {
      impl_->access = std::make_shared<AccessController>(config.access);
      if (!impl_->access->start(requiredAccessAsgs(config), error)) return false;
      impl_->server.addSource("access", impl_->access->source());
    }
    impl_->admin = std::make_unique<AdminNamespace>(config.server, config.access.enabled);
    impl_->admin->install(impl_->server, impl_->access.get(), config.access.defaults);
    impl_->admin->setAccessStatus(impl_->access ? impl_->access->status() : AccessStatus{});
    if (!applyConfig(config, true, error)) {
      return false;
    }
    impl_->server.start();
    if (config.discovery.enabled) {
      impl_->discovery = std::make_unique<DiscoveryPublisher>(config.discovery);
      impl_->discovery->publish(impl_->catalog);
      impl_->admin->setDiscoveryStatus(impl_->discovery->status());
    }
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
  if (impl_->access && impl_->admin && impl_->admin->consumeAccessReloadRequest()) {
    impl_->access->requestReload("admin-pv");
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

  if (impl_->access) impl_->access->pump();

  const auto now = std::chrono::steady_clock::now();
  if (now - impl_->lastHealthUpdate >= std::chrono::seconds(1)) {
    impl_->lastHealthUpdate = now;
    if (impl_->admin) {
      impl_->admin->setBackendHealth(backendHealthSummary(impl_->redisBackends));
      impl_->admin->setDiscoveryStatus(impl_->discovery ? impl_->discovery->status() : DiscoveryStatus{});
      impl_->admin->setAccessStatus(impl_->access ? impl_->access->status() : AccessStatus{});
    }
  }
}

void Application::stop() {
  if (!started_) {
    return;
  }

  impl_->discovery.reset();
  impl_->catalog.reset();
  for (const auto& binding : pvBindings(impl_->currentConfig)) {
    impl_->removeEndpoint(binding.first);
  }
  for (auto& item : impl_->runtimes) {
    item.second->deactivate("application stopping");
  }
  impl_->runtimes.clear();
  for (auto& item : impl_->rpcPVs) {
    impl_->removeEndpoint(item.first);
  }
  impl_->rpcPVs.clear();
  impl_->rpcServices.clear();
  impl_->redisBackends.clear();
  impl_->alarmPublisher.reset();

  if (impl_->admin) {
    impl_->admin->remove(impl_->server, impl_->access.get());
  }
  if (impl_->access) {
    impl_->server.removeSource("access");
    impl_->access.reset();
  }
  impl_->server.stop();
  started_ = false;
}

bool Application::applyConfig(const AppConfig& config, const bool initialLoad, std::string& error) {
  if (!initialLoad && impl_->hasConfig && impl_->admin)
    impl_->admin->setLastDiff(formatConfigDiff(diffConfigs(impl_->currentConfig, config), true));
  if (!initialLoad && impl_->hasConfig && !sameServerConfig(impl_->currentConfig.server, config.server)) {
    error = "server namespace/bind settings are immutable after startup";
    return false;
  }
  if (!initialLoad && impl_->hasConfig &&
      impl_->currentConfig.access.enabled != config.access.enabled) {
    error = "access.enabled is immutable after startup; restart is required";
    return false;
  }

  if (!initialLoad && !sameDiscoveryConfig(impl_->currentConfig.discovery, config.discovery)) {
    error = "discovery listener settings are immutable after startup; restart is required";
    return false;
  }
  bool policyActivated = false;
  const BeforeCommit activatePolicy = [&](std::string& activationError) {
    if (initialLoad || !impl_->access) return true;
    if (!impl_->access->reconfigure(config.access, requiredAccessAsgs(config), activationError)) {
      return false;
    }
    policyActivated = true;
    try {
      impl_->admin->setAccessAssignments(*impl_->access, config.access.defaults);
      return true;
    } catch (const std::exception& ex) {
      activationError = ex.what();
      return false;
    }
  };

  const auto nextGeneration = impl_->generation + 1;

  const bool applied = applyIncremental(config, nextGeneration, activatePolicy, error);
  if (!applied && policyActivated) {
    std::string restoreError;
    if (!impl_->access->restorePrevious(restoreError)) {
      error += "; previous ACF restore failed: " + restoreError;
    } else {
      impl_->admin->setAccessAssignments(*impl_->access, impl_->currentConfig.access.defaults);
    }
  }
  return applied;
}

bool Application::applyIncremental(const AppConfig& config,
                                   const uint64_t generation,
                                   const BeforeCommit& beforeCommit,
                                   std::string& error) {
  try {
    auto newRedisBackends = buildRedisBackends(config, impl_->currentConfig, impl_->redisBackends);
    const auto priorAlarm = impl_->currentConfig.redisBackends.find(config.alarms.backend);
    const bool reuseAlarm = impl_->hasConfig &&
        sameAlarmStreamConfig(impl_->currentConfig.alarms, config.alarms) &&
        priorAlarm != impl_->currentConfig.redisBackends.end() &&
        sameRedisConfig(priorAlarm->second, config.redisBackends.at(config.alarms.backend));
    auto newAlarmPublisher = reuseAlarm ? impl_->alarmPublisher : buildAlarmPublisher(config);
    const auto currentBindings = pvBindings(impl_->currentConfig);
    const auto desiredBindings = pvBindings(config);
    std::map<std::string, PVConfig> desired;
    for (const auto& pv : config.pvs) {
      desired.emplace(fullPVName(config.server, pv), pv);
    }

    std::vector<std::string> removeNames;
    std::vector<std::string> replaceNames;
    std::vector<std::pair<std::string, PVConfig>> addNames;
    std::vector<std::pair<std::string, PVConfig>> reconfigureNames;
    std::set<std::string> reopenNames;

    for (const auto& current : impl_->runtimes) {
      const auto desiredIt = desired.find(current.first);
      if (desiredIt == desired.end()) {
        removeNames.push_back(current.first);
        continue;
      }

      if (current.second->structurallyCompatible(desiredIt->second) &&
          sameBackendBindings(desiredIt->second, impl_->redisBackends, newRedisBackends)) {
        reconfigureNames.emplace_back(current.first, desiredIt->second);
        const auto currentServedNames =
            fullPVNames(impl_->currentConfig.server, current.second->config());
        const auto desiredServedNames = fullPVNames(config.server, desiredIt->second);
        if (std::set<std::string>(currentServedNames.begin(), currentServedNames.end()) !=
            std::set<std::string>(desiredServedNames.begin(), desiredServedNames.end())) {
          reopenNames.insert(current.first);
        }
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
    // Owned subscriptions let staged runtimes attach independently. Do not
    // suspend every live reader while constructing an unrelated runtime.
    for (const auto& name : replaceNames)
      staged.emplace(name, makeRuntime(config.server, desired.at(name), newRedisBackends, newAlarmPublisher, generation));
    for (const auto& item : addNames)
      staged.emplace(item.first, makeRuntime(config.server, item.second, newRedisBackends, newAlarmPublisher, generation));

    const std::set<std::string> replaced(replaceNames.begin(), replaceNames.end());
    std::map<std::string, pvxs::Value> reopenValues;
    for (const auto& name : reopenNames) {
      reopenValues.emplace(name, impl_->runtimes.at(name)->sharedPV().fetch());
    }

    AssignmentMap stagedRpcAssignments;
    RpcServices stagedRpcServices;
    auto stagedRpcPVs = buildRpcPVs(config, stagedRpcAssignments, impl_->rpcServices, stagedRpcServices);
    auto stagedCatalog = buildDiscoveryCatalog(config, stagedRpcPVs, impl_->server.config().tcp_port, generation);
    if (!beforeCommit(error)) {
      for (auto& item : staged) item.second->deactivate("staged config rejected");
      return false;
    }

    for (const auto& binding : currentBindings) {
      const auto desiredIt = desiredBindings.find(binding.first);
      if (desiredIt == desiredBindings.end() ||
          desiredIt->second != binding.second ||
          replaced.count(binding.second) != 0u ||
          reopenNames.count(binding.second) != 0u) {
        impl_->removeEndpoint(binding.first);
      }
    }

    // PVXS StaticSource::remove() closes the SharedPV even when that same
    // SharedPV is registered under other names. Alias-only changes therefore
    // remove every binding for the logical runtime, reopen the existing
    // SharedPV with its current value, and then register the desired name set.
    // The Redis readers and confirmation routes remain attached to the same
    // runtime throughout.
    for (const auto& name : reopenNames) {
      impl_->runtimes.at(name)->sharedPV().open(reopenValues.at(name));
    }

    for (const auto& item : reconfigureNames) {
      impl_->runtimes.at(item.first)->setAlarmPublisher(newAlarmPublisher);
      impl_->runtimes.at(item.first)->reconfigure(item.second, generation);
      for (const auto& servedName : fullPVNames(config.server, item.second)) {
        const auto currentIt = currentBindings.find(servedName);
        if (currentIt != currentBindings.end() &&
            currentIt->second == item.first &&
            reopenNames.count(item.first) == 0u) {
          impl_->setAssignment(servedName, item.second.access.value_or(config.access.defaults.pv));
        }
      }
    }

    for (const auto& name : removeNames) {
      impl_->runtimes.at(name)->deactivate("pv removed");
      impl_->runtimes.erase(name);
    }

    for (const auto& name : replaceNames) {
      impl_->runtimes.at(name)->deactivate("pv replaced");
      impl_->runtimes.erase(name);
      impl_->runtimes.emplace(name, staged.at(name));
      staged.at(name)->activate();
      staged.erase(name);
    }

    for (auto& item : staged) {
      item.second->activate();
      impl_->runtimes.emplace(item.first, item.second);
    }

    for (const auto& binding : desiredBindings) {
      const auto currentIt = currentBindings.find(binding.first);
      if (currentIt == currentBindings.end() ||
          currentIt->second != binding.second ||
          replaced.count(binding.second) != 0u ||
          reopenNames.count(binding.second) != 0u) {
        impl_->addEndpoint(
            binding.first,
            impl_->runtimes.at(binding.second)->sharedPV(),
            desired.at(binding.second).access.value_or(config.access.defaults.pv));
      }
    }

    // Preserve unchanged reflected services and their existing client channels.
    for (auto& item : impl_->rpcPVs) {
      const auto next = stagedRpcPVs.find(item.first);
      if (next == stagedRpcPVs.end() || next->second != item.second)
        impl_->removeEndpoint(item.first);
    }
    auto priorRpcPVs = std::move(impl_->rpcPVs);
    impl_->rpcPVs = std::move(stagedRpcPVs);
    impl_->rpcAssignments = std::move(stagedRpcAssignments);
    impl_->rpcServices = std::move(stagedRpcServices);
    for (auto& item : impl_->rpcPVs) {
      const auto previous = priorRpcPVs.find(item.first);
      if (previous != priorRpcPVs.end() && previous->second == item.second)
        impl_->setAssignment(item.first, impl_->rpcAssignments.at(item.first));
      else
        impl_->addEndpoint(item.first, *item.second, impl_->rpcAssignments.at(item.first));
    }

    impl_->redisBackends = std::move(newRedisBackends);
    impl_->alarmPublisher = std::move(newAlarmPublisher);
    impl_->currentConfig = config;
    impl_->hasConfig = true;
    impl_->generation = generation;
    impl_->catalog = std::move(stagedCatalog);
    if (impl_->discovery) impl_->discovery->publish(impl_->catalog);

    if (impl_->admin) {
      impl_->admin->setGeneration(generation);
      impl_->admin->setPvCount(impl_->runtimes.size() + impl_->rpcPVs.size());
      impl_->admin->setStatus("generation " + std::to_string(generation) + " active");
      impl_->admin->setError("");
      impl_->admin->setBackendHealth(backendHealthSummary(impl_->redisBackends));
    }
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

}  // namespace redis_pvxs_ioc
