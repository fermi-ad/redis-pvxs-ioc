#include "redis_pvxs_ioc/config_diff.h"
#include <iomanip>
#include <algorithm>
#include <set>
#include <sstream>
#include <tuple>

namespace redis_pvxs_ioc {
namespace {
bool sameMetadata(const MetadataConfig& a, const MetadataConfig& b) {
  return std::tie(a.description, a.units, a.precision, a.form, a.display.low, a.display.high,
                  a.control.low, a.control.high, a.minStep) ==
         std::tie(b.description, b.units, b.precision, b.form, b.display.low, b.display.high,
                  b.control.low, b.control.high, b.minStep);
}
bool sameAlarms(const AlarmConfig& a, const AlarmConfig& b) {
  return std::tie(a.lowAlarm, a.lowWarning, a.highWarning, a.highAlarm, a.hysteresis) ==
         std::tie(b.lowAlarm, b.lowWarning, b.highWarning, b.highAlarm, b.hysteresis);
}
bool sameTransform(const std::optional<LinearTransformConfig>& a, const std::optional<LinearTransformConfig>& b) {
  return (!a && !b) || (a && b && a->scale == b->scale && a->offset == b->offset);
}
bool sameAssignment(const std::optional<AccessAssignment>& a, const std::optional<AccessAssignment>& b) {
  return (!a && !b) || (a && b && sameAccessAssignment(*a, *b));
}
bool sameRpc(const RpcServiceConfig& a, const RpcServiceConfig& b) {
  return std::tie(a.service, a.endpoint, a.suffix, a.defaults) == std::tie(b.service, b.endpoint, b.suffix, b.defaults)
      && sameAssignment(a.access, b.access);
}
std::set<std::string> aliasSet(const PVConfig& pv) { return {pv.aliases.begin(), pv.aliases.end()}; }
} // namespace

ConfigDiff diffConfigs(const AppConfig& before, const AppConfig& after) {
  ConfigDiff diff;
  if (before.server.instance != after.server.instance) diff.restartRequired.push_back("server.instance");
  if (before.server.nameSpace != after.server.nameSpace) diff.restartRequired.push_back("server.namespace");
  if (before.server.interfaces != after.server.interfaces) diff.restartRequired.push_back("server.interfaces");
  if (before.server.tcpPort != after.server.tcpPort) diff.restartRequired.push_back("server.tcp_port");
  if (before.server.udpPort != after.server.udpPort) diff.restartRequired.push_back("server.udp_port");
  if (before.server.autoBeacon != after.server.autoBeacon) diff.restartRequired.push_back("server.auto_beacon");
  if (before.access.enabled != after.access.enabled) diff.restartRequired.push_back("access.enabled");
  if (!sameDiscoveryConfig(before.discovery, after.discovery)) diff.restartRequired.push_back("discovery");
  if (!sameAccessConfig(before.access, after.access)) diff.accessChanged.push_back("policy/defaults/watcher");

  std::map<std::string, const PVConfig*> oldPVs, newPVs;
  for (const auto& pv : before.pvs) oldPVs.emplace(fullPVName(before.server, pv), &pv);
  for (const auto& pv : after.pvs) newPVs.emplace(fullPVName(after.server, pv), &pv);
  for (const auto& item : oldPVs) {
    const auto next = newPVs.find(item.first);
    if (next == newPVs.end()) { diff.removed.push_back(item.first); continue; }
    const auto& a = *item.second;
    const auto& b = *next->second;
    if (!sameReaderTopology(a, b) || aliasSet(a) != aliasSet(b)) diff.replaced.push_back(item.first);
    else if (!sameMetadata(a.metadata, b.metadata) || !sameAlarms(a.alarms, b.alarms)
             || !sameTransform(a.transform, b.transform) || a.initialValue != b.initialValue)
      diff.metadataChanged.push_back(item.first);
    if (!sameAssignment(a.access, b.access)) diff.accessChanged.push_back(item.first);
  }
  for (const auto& item : newPVs) if (!oldPVs.count(item.first)) diff.added.push_back(item.first);

  std::set<std::string> backends;
  for (const auto& entry : before.redisBackends) backends.insert(entry.first);
  for (const auto& entry : after.redisBackends) backends.insert(entry.first);
  for (const auto& name : backends) {
    const auto a = before.redisBackends.find(name), b = after.redisBackends.find(name);
    if (a == before.redisBackends.end() || b == after.redisBackends.end() || !sameRedisConfig(a->second, b->second))
      diff.backendsChanged.push_back(name); // Never include credentials or changed values.
  }
  for (const auto& item : newPVs) {
    if (!oldPVs.count(item.first)) continue;
    const auto& pv = *item.second;
    const auto changed = [&](const std::string& name) {
      return std::find(diff.backendsChanged.begin(), diff.backendsChanged.end(), name) != diff.backendsChanged.end();
    };
    if (changed(pv.read.backend) || (pv.write && changed(pv.write->backend)) || (pv.confirm && changed(pv.confirm->backend))) {
      if (std::find(diff.replaced.begin(), diff.replaced.end(), item.first) == diff.replaced.end()) diff.replaced.push_back(item.first);
      diff.metadataChanged.erase(std::remove(diff.metadataChanged.begin(), diff.metadataChanged.end(), item.first), diff.metadataChanged.end());
    }
  }
  std::sort(diff.replaced.begin(), diff.replaced.end());
  bool rpcEqual = before.rpcServices.size() == after.rpcServices.size();
  if (rpcEqual) for (size_t i = 0; i < before.rpcServices.size(); ++i)
    if (!sameRpc(before.rpcServices[i], after.rpcServices[i])) { rpcEqual = false; break; }
  if (!rpcEqual) {
    std::set<std::string> services;
    for (const auto& service : before.rpcServices) services.insert(service.service + service.suffix);
    for (const auto& service : after.rpcServices) services.insert(service.service + service.suffix);
    diff.rpcServicesChanged.assign(services.begin(), services.end());
  }
  diff.alarmStreamChanged = !sameAlarmStreamConfig(before.alarms, after.alarms);
  diff.catalogMetadataChanged = std::tie(before.channelFinder.url, before.channelFinder.owner,
      before.channelFinder.tags, before.channelFinder.properties) != std::tie(after.channelFinder.url,
      after.channelFinder.owner, after.channelFinder.tags, after.channelFinder.properties);
  return diff;
}

std::string quoteJson(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    switch (c) {
    case '"': out << "\\\""; break;
    case '\\': out << "\\\\"; break;
    case '\n': out << "\\n"; break;
    case '\r': out << "\\r"; break;
    case '\t': out << "\\t"; break;
    default:
      if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
      else out << c;
    }
  }
  return out.str() + '"';
}

std::string formatConfigDiff(const ConfigDiff& diff, bool json) {
  std::ostringstream out;
  if (json) out << '{';
  bool first = true;
  const auto list = [&](const char* name, const std::vector<std::string>& values) {
    if (json) {
      if (!first) out << ',';
      first = false;
      out << quoteJson(name) << ":[";
      for (size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << quoteJson(values[i]); }
      out << ']';
    } else {
      out << name << " (" << values.size() << ")";
      for (const auto& value : values) out << "\n  " << value;
      out << '\n';
    }
  };
  list("additions", diff.added); list("removals", diff.removed); list("replacements", diff.replaced);
  list("metadata_changes", diff.metadataChanged); list("access_changes", diff.accessChanged);
  list("backend_changes", diff.backendsChanged); list("rpc_service_changes", diff.rpcServicesChanged);
  list("restart_required", diff.restartRequired);
  if (json) out << ",\"alarm_stream_changed\":" << (diff.alarmStreamChanged ? "true" : "false")
                << ",\"catalog_metadata_changed\":" << (diff.catalogMetadataChanged ? "true" : "false") << '}';
  else out << "alarm_stream_changed: " << (diff.alarmStreamChanged ? "yes" : "no")
           << "\ncatalog_metadata_changed: " << (diff.catalogMetadataChanged ? "yes" : "no") << '\n';
  return out.str();
}
} // namespace redis_pvxs_ioc
