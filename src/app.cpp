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
#include "redis_pvxs_ioc/operation_queue.h"
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

class AdminNamespace : public std::enable_shared_from_this<AdminNamespace> {
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
    operationsName_ = adminPVName(serverConfig, "stats:operations");
    operations_.open(pvxs::TypeDef(TypeCode::Struct, "redis-pvxs-ioc:operations:1.0", {
      Member(TypeCode::UInt64, "accepted"), Member(TypeCode::UInt64, "finished"),
      Member(TypeCode::UInt64, "overloaded"), Member(TypeCode::UInt64, "expired"),
      Member(TypeCode::UInt64, "cancelled"), Member(TypeCode::UInt64, "queued"),
      Member(TypeCode::UInt64, "running"), Member(TypeCode::UInt64, "residentBytes"),
      Member(TypeCode::UInt64, "peakBytes"), Member(TypeCode::UInt64, "byteLimit"),
      Member(TypeCode::UInt64, "payloadLimit"), Member(TypeCode::UInt32, "workers"),
      Member(TypeCode::UInt32, "queuedPerPV")}).create());
    alarmName_ = adminPVName(serverConfig, "alarms:status");
    alarms_.open(pvxs::TypeDef(TypeCode::Struct, "redis-pvxs-ioc:alarms:1.0", {
      Member(TypeCode::String, "state"), Member(TypeCode::String, "lastError"),
      Member(TypeCode::UInt64, "sent"), Member(TypeCode::UInt64, "transportFailures"),
      Member(TypeCode::UInt64, "rejected"), Member(TypeCode::UInt64, "reconciled"),
      Member(TypeCode::UInt64, "coalescedUpdates"), Member(TypeCode::UInt64, "discardedTransitions"),
      Member(TypeCode::UInt64, "messagesTruncated"), Member(TypeCode::UInt64, "registrations"),
      Member(TypeCode::UInt64, "active"), Member(TypeCode::UInt64, "pending"),
      Member(TypeCode::UInt64, "queued"), Member(TypeCode::UInt64, "reservedBytes"),
      Member(TypeCode::UInt64, "peakBytes"), Member(TypeCode::UInt64, "byteLimit"),
      Member(TypeCode::UInt64, "queueLimit")}).create());
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

  void setOperations(const OperationQueueStats& stats, const OperationLimitsConfig& limits) {
    auto value = operations_.fetch();
    value["accepted"] = stats.accepted; value["finished"] = stats.completed;
    value["overloaded"] = stats.overloaded; value["expired"] = stats.expired;
    value["cancelled"] = stats.cancelled; value["queued"] = static_cast<uint64_t>(stats.queued);
    value["running"] = static_cast<uint64_t>(stats.running);
    value["residentBytes"] = static_cast<uint64_t>(stats.residentBytes);
    value["peakBytes"] = static_cast<uint64_t>(stats.peakBytes);
    value["byteLimit"] = limits.queuedWriteBytes; value["payloadLimit"] = limits.maxPayloadBytes;
    value["workers"] = limits.writeWorkers; value["queuedPerPV"] = limits.queuedWritesPerPV;
    operations_.post(value);
  }

  void setDiscoveryStatus(const DiscoveryStatus& status) {
    auto value = discovery_.fetch();
    value["state"] = status.state; value["peer"] = status.peer; value["lastError"] = status.lastError;
    value["desiredGeneration"] = status.desiredGeneration; value["synchronizedGeneration"] = status.synchronizedGeneration;
    value["records"] = status.records; value["aliases"] = status.aliases; value["bytes"] = status.bytes;
    value["uploads"] = status.uploads; value["failures"] = status.failures; value["coalesced"] = status.coalesced;
    value["invalidAnnouncements"] = status.invalidAnnouncements; value["port"] = status.port;
    discovery_.post(value);
  }

  void setAlarmStatus(const AlarmPublisherStatus& status) {
    auto value = alarms_.fetch();
    value["state"] = status.state; value["lastError"] = status.lastError;
    value["sent"] = status.sent; value["transportFailures"] = status.transportFailures;
    value["rejected"] = status.rejected; value["reconciled"] = status.reconciled;
    value["coalescedUpdates"] = status.coalescedUpdates; value["discardedTransitions"] = status.discardedTransitions;
    value["messagesTruncated"] = status.messagesTruncated; value["registrations"] = status.registrations;
    value["active"] = status.active; value["pending"] = status.pending; value["queued"] = status.queued;
    value["reservedBytes"] = status.reservedBytes; value["peakBytes"] = status.peakBytes;
    value["byteLimit"] = status.byteLimit; value["queueLimit"] = status.queueLimit;
    alarms_.post(value);
  }

  PVBindings bindings(const AccessDefaultsConfig& defaults) {
    PVBindings bindings;
    EndpointRegistry endpoints;
    const auto add = [&](const std::string& name, const pvxs::server::SharedPV& pv,
                         const AccessAssignment& assignment) {
      endpoints.reserve(name, EndpointKind::Diagnostic, "runtime");
      bindings.emplace(name, PVBinding{pv, shared_from_this(), assignment});
    };
    add(lastDiffName_, lastDiff_, defaults.adminRead);
    add(operationsName_, operations_, defaults.adminRead);
    add(alarmName_, alarms_, defaults.adminRead);
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
    return bindings;
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
  pvxs::server::SharedPV operations_ = pvxs::server::SharedPV::buildReadonly();
  std::string operationsName_;
  pvxs::server::SharedPV alarms_ = pvxs::server::SharedPV::buildReadonly();
  std::string alarmName_;
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
  AlarmPublisherOptions options;
  options.queueEntries = config.limits.alarmQueueEntries;
  options.stateBytes = config.limits.alarmStateBytes;
  return std::make_shared<AlarmPublisher>(backend.host, backend.port, config.alarms.stream, backend.user, backend.password, options);
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
using AssignmentMap = std::unordered_map<std::string, AccessAssignment>;
struct RpcServiceState {
  RpcServiceConfig config;
  RpcMap pvs;
  std::map<std::string, std::string> owners;
};
using RpcServices = std::vector<RpcServiceState>;

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
  std::shared_ptr<OperationQueue> operations;
  pvxs::server::Server server;
  std::unique_ptr<DiscoveryPublisher> discovery;
  std::shared_ptr<const DiscoveryCatalog> catalog;
  std::shared_ptr<AccessController> access;
  std::shared_ptr<AdminNamespace> admin;
  std::shared_ptr<PVRegistry> registry = std::make_shared<PVRegistry>();
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
};

Application::Application(std::string configPath)
    : configPath_(std::move(configPath)),
      impl_(std::make_unique<Impl>()) {}

Application::~Application() {
  stop();
  if (impl_->operations) impl_->operations->shutdown();
}

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
    impl_->operations = std::make_shared<OperationQueue>(OperationQueueLimits{
        config.limits.writeWorkers, config.limits.queuedWritesPerPV, static_cast<size_t>(config.limits.queuedWriteBytes)});
    impl_->server = buildServerConfig(config).build();
    if (config.access.enabled) {
      impl_->access = std::make_shared<AccessController>(config.access);
      if (!impl_->access->start(requiredAccessAsgs(config), error)) return false;
      impl_->server.addSource("access", impl_->access->source());
    }
    impl_->admin = std::make_shared<AdminNamespace>(config.server, config.access.enabled);
    if (!impl_->access) impl_->server.addSource("registry", impl_->registry);
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
      impl_->admin->setOperations(impl_->operations->stats(), impl_->currentConfig.limits);
      impl_->admin->setAlarmStatus(impl_->alarmPublisher->status());
      impl_->admin->setDiscoveryStatus(impl_->discovery ? impl_->discovery->status() : DiscoveryStatus{});
      impl_->admin->setAccessStatus(impl_->access ? impl_->access->status() : AccessStatus{});
    }
  }
}

void Application::stop() {
  if (!started_ && !impl_->hasConfig) return;
  impl_->discovery.reset();
  impl_->catalog.reset();
  if (impl_->access) impl_->access->clearBindings();
  else impl_->registry->clear();
  for (auto& item : impl_->runtimes) item.second->deactivate("application stopping");
  if (impl_->operations) impl_->operations->shutdown();
  impl_->runtimes.clear();
  impl_->rpcPVs.clear();
  impl_->rpcServices.clear();
  impl_->redisBackends.clear();
  impl_->alarmPublisher.reset();
  if (impl_->access) {
    impl_->server.removeSource("access");
  } else {
    impl_->server.removeSource("registry");
  }
  impl_->server.stop();
  impl_->access.reset();
  impl_->hasConfig = false;
  started_ = false;
}

bool Application::applyConfig(const AppConfig& config, const bool initialLoad, std::string& error) {
  if (!initialLoad && impl_->hasConfig && impl_->admin)
    impl_->admin->setLastDiff(formatConfigDiff(diffConfigs(impl_->currentConfig, config), true));
  if (!initialLoad && !sameOperationLimits(impl_->currentConfig.limits, config.limits)) {
    error = "operation limits are immutable after startup; restart is required";
    return false;
  }
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
  return applyGeneration(config, impl_->generation + 1, error);
}

bool Application::applyGeneration(const AppConfig& config,
                                   const uint64_t generation,
                                   std::string& error) {
  bool committed = false;
  try {
    AppConfig nextConfig = config;
    auto nextBackends = buildRedisBackends(config, impl_->currentConfig, impl_->redisBackends);
    const auto priorAlarm = impl_->currentConfig.redisBackends.find(config.alarms.backend);
    const bool reuseAlarm = impl_->hasConfig &&
        sameAlarmStreamConfig(impl_->currentConfig.alarms, config.alarms) &&
        priorAlarm != impl_->currentConfig.redisBackends.end() &&
        sameRedisConfig(priorAlarm->second, config.redisBackends.at(config.alarms.backend));
    auto nextAlarm = reuseAlarm ? impl_->alarmPublisher : buildAlarmPublisher(config);

    RuntimeMap nextRuntimes;
    std::vector<std::unique_ptr<PVRuntimeUpdate>> updates;
    std::vector<std::shared_ptr<PVRuntimeBase>> added, retired;
    for (const auto& pv : config.pvs) {
      const auto name = fullPVName(config.server, pv);
      const auto existing = impl_->runtimes.find(name);
      if (existing != impl_->runtimes.end() && existing->second->structurallyCompatible(pv) &&
          sameBackendBindings(pv, impl_->redisBackends, nextBackends)) {
        updates.emplace_back(existing->second->prepareReconfigure(pv, generation, nextAlarm));
        nextRuntimes.emplace(name, existing->second);
      } else {
        auto runtime = makeRuntime(config.server, pv, nextBackends, nextAlarm, generation,
                                   impl_->operations, config.limits);
        added.push_back(runtime);
        nextRuntimes.emplace(name, std::move(runtime));
      }
    }
    for (const auto& old : impl_->runtimes) {
      const auto next = nextRuntimes.find(old.first);
      if (next == nextRuntimes.end() || next->second != old.second) retired.push_back(old.second);
    }
    AssignmentMap nextRpcAssignments;
    RpcServices nextRpcServices;
    auto nextRpcs = buildRpcPVs(config, nextRpcAssignments, impl_->rpcServices, nextRpcServices);
    auto nextCatalog = buildDiscoveryCatalog(config, nextRpcs, impl_->server.config().tcp_port, generation);

    auto bindings = impl_->admin->bindings(config.access.defaults);
    const auto bind = [&](const std::string& name, const pvxs::server::SharedPV& pv,
                          std::shared_ptr<void> owner, const AccessAssignment& assignment) {
      if (!bindings.emplace(name, PVBinding{pv, std::move(owner), assignment}).second)
        throw std::runtime_error("duplicate endpoint binding: " + name);
    };
    for (const auto& pv : config.pvs) {
      const auto runtime = nextRuntimes.at(fullPVName(config.server, pv));
      for (const auto& name : fullPVNames(config.server, pv))
        bind(name, runtime->sharedPV(), runtime, pv.access.value_or(config.access.defaults.pv));
    }
    for (const auto& rpc : nextRpcs)
      bind(rpc.first, *rpc.second, rpc.second, nextRpcAssignments.at(rpc.first));

    std::shared_ptr<AccessController::PreparedConfiguration> policy;
    std::shared_ptr<AccessController::PreparedBindings> secured;
    std::shared_ptr<PVRegistry::Prepared> plain;
    if (impl_->access) {
      if (impl_->hasConfig) policy = impl_->access->prepareConfiguration(config.access, requiredAccessAsgs(config));
      secured = impl_->access->prepareBindings(bindings);
    } else {
      plain = impl_->registry->prepare(std::move(bindings));
    }

    // All allocation, reflection, metadata validation and endpoint construction
    // precede this point. Policy activation is the last fallible commit action;
    // a rejected policy leaves the live values, members and channels untouched.
    const auto commit = [&](std::string& failure) {
      if (policy && !impl_->access->activateConfiguration(policy, failure)) return false;
      for (const auto& update : updates) update->commit();
      for (const auto& runtime : added) runtime->activate();
      return true;
    };
    const bool published = impl_->access
        ? impl_->access->publishBindings(secured, commit, error)
        : impl_->registry->publish(plain, commit, error);
    if (!published) return false;
    committed = true;

    // Publication has committed. No subsequent failure is reported as a rejected
    // generation. Retain old owners until retired channels/work have drained.
    impl_->runtimes.swap(nextRuntimes);
    impl_->redisBackends.swap(nextBackends);
    impl_->rpcPVs.swap(nextRpcs);
    impl_->rpcAssignments.swap(nextRpcAssignments);
    impl_->rpcServices.swap(nextRpcServices);
    impl_->alarmPublisher.swap(nextAlarm);
    std::swap(impl_->currentConfig, nextConfig);
    impl_->catalog.swap(nextCatalog);
    impl_->generation = generation;
    impl_->hasConfig = true;
    try {
      if (secured) impl_->access->finishBindings(secured);
      else impl_->registry->finish(plain);
      for (const auto& runtime : retired) runtime->deactivate("generation retired");
      if (!reuseAlarm && nextAlarm) nextAlarm->stop();
      for (const auto& update : updates) update->refresh();
      impl_->alarmPublisher->activate();
      if (policy) impl_->access->finishConfiguration();
      if (impl_->discovery) impl_->discovery->publish(impl_->catalog);
      impl_->admin->setGeneration(generation);
      impl_->admin->setPvCount(impl_->runtimes.size() + impl_->rpcPVs.size());
      impl_->admin->setStatus("generation " + std::to_string(generation) + " active");
      impl_->admin->setError("");
      impl_->admin->setOperations(impl_->operations->stats(), impl_->currentConfig.limits);
      impl_->admin->setAlarmStatus(impl_->alarmPublisher->status());
    } catch (const std::exception& ex) {
      // The active registry is already complete. Preserve its generation and
      // report an operational refresh failure, not a fictitious rollback.
      std::fprintf(stderr, "[redis-pvxs-ioc] generation %llu committed; refresh failed: %s\n",
                   static_cast<unsigned long long>(generation), ex.what());
      impl_->admin->setGeneration(generation);
      impl_->admin->setStatus("generation active; refresh failed");
      impl_->admin->setError(ex.what());
    }
    error.clear();
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    if (committed)
      std::fprintf(stderr, "[redis-pvxs-ioc] committed generation %llu reporting failed: %s\n",
                   static_cast<unsigned long long>(generation), ex.what());
    return committed;
  }
}

}  // namespace redis_pvxs_ioc
