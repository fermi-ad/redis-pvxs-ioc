#include "redis_pvxs_ioc/config.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <cctype>
#include <filesystem>
#include <arpa/inet.h>
#include <tuple>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace redis_pvxs_ioc {
namespace {

std::string lowerCopy(const std::string& input) {
  std::string lowered(input);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

[[noreturn]] void fail(const std::string& path, const std::string& message) {
  throw std::runtime_error(path + ": " + message);
}

YAML::Node requireNode(const YAML::Node& parent, const char* key, const std::string& path) {
  const auto node = parent[key];
  if (!node) {
    fail(path, std::string("missing required key '") + key + "'");
  }
  return node;
}

YAML::Node requireMap(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsMap()) {
    fail(path, "expected mapping");
  }
  return node;
}

YAML::Node requireSequence(const YAML::Node& node, const std::string& path) {
  if (!node || !node.IsSequence()) {
    fail(path, "expected sequence");
  }
  return node;
}

void rejectUnknownKeys(const YAML::Node& node,
                       const std::string& path,
                       const std::set<std::string>& allowed) {
  for (const auto& entry : requireMap(node, path)) {
    const auto key = entry.first.as<std::string>();
    if (allowed.count(key) == 0u) fail(path + "." + key, "unknown key");
  }
}

void validateTree(const YAML::Node& node, const std::string& path,
                  std::vector<YAML::Node>& ancestors, size_t& visited) {
  if (++visited > 1000000 || ancestors.size() >= 64) fail(path, "configuration structure exceeds validation bounds");
  for (const auto& ancestor : ancestors)
    if (node.is(ancestor)) fail(path, "recursive YAML aliases are unsupported");
  if (node.IsMap()) {
    ancestors.push_back(node);
    std::set<std::string> keys;
    for (const auto& entry : node) {
      if (!entry.first.IsScalar()) fail(path, "mapping keys must be scalar strings");
      const auto key = entry.first.as<std::string>();
      if (!keys.insert(key).second) fail(path + "." + key, "duplicate key");
      validateTree(entry.second, path + "." + key, ancestors, visited);
    }
    ancestors.pop_back();
  } else if (node.IsSequence()) {
    ancestors.push_back(node);
    for (size_t i = 0; i < node.size(); ++i) validateTree(node[i], path + "[" + std::to_string(i) + "]", ancestors, visited);
    ancestors.pop_back();
  }
}

template <typename T>
T parseNumeric(const YAML::Node& node, const std::string& path) {
  if constexpr (std::is_unsigned_v<T> && !std::is_same_v<T, bool>) {
    const auto text = node.Scalar();
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && text[first] == '-') fail(path, "negative unsigned value");
  }
  T value{};
  try {
    value = node.as<T>();
  } catch (const std::exception& ex) {
    fail(path, ex.what());
  }
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value)) fail(path, "must be finite");
  }
  return value;
}

template <>
int8_t parseNumeric<int8_t>(const YAML::Node& node, const std::string& path) {
  const auto value = parseNumeric<int>(node, path);
  if (value < -128 || value > 127) fail(path, "outside int8 range");
  return static_cast<int8_t>(value);
}

template <>
uint8_t parseNumeric<uint8_t>(const YAML::Node& node, const std::string& path) {
  const auto value = parseNumeric<unsigned int>(node, path);
  if (value > 255) fail(path, "outside uint8 range");
  return static_cast<uint8_t>(value);
}

std::string parseString(const YAML::Node& node, const std::string& path) {
  try {
    return node.as<std::string>();
  } catch (const std::exception& ex) {
    fail(path, ex.what());
  }
}

AccessAssignment parseAccessAssignment(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"asg", "asl"});
  AccessAssignment assignment;
  if (node["asg"]) {
    assignment.asg = parseString(node["asg"], path + ".asg");
  }
  if (node["asl"]) {
    assignment.asl = parseNumeric<int>(node["asl"], path + ".asl");
  }
  if (assignment.asg.empty()) {
    fail(path + ".asg", "must not be empty");
  }
  if (assignment.asl != 0 && assignment.asl != 1) {
    fail(path + ".asl", "must be 0 or 1");
  }
  return assignment;
}

AccessConfig parseAccessConfig(const YAML::Node& node,
                               const std::filesystem::path& configDirectory,
                               const std::string& path) {
  AccessConfig config;
  if (!node) {
    return config;
  }

  rejectUnknownKeys(node, path, {"enabled", "file", "macros", "watch", "defaults"});
  if (node["enabled"]) {
    config.enabled = node["enabled"].as<bool>();
  }
  if (node["file"]) {
    config.file = parseString(node["file"], path + ".file");
  }
  if (node["macros"]) {
    const auto macros = requireMap(node["macros"], path + ".macros");
    for (const auto& entry : macros) {
      const auto name = parseString(entry.first, path + ".macros.<name>");
      if (name.empty()) {
        fail(path + ".macros", "macro name must not be empty");
      }
      config.macros[name] = parseString(entry.second, path + ".macros." + name);
    }
  }
  if (node["watch"]) {
    const auto watch = requireMap(node["watch"], path + ".watch");
    rejectUnknownKeys(watch, path + ".watch", {"enabled", "interval_ms", "settle_ms"});
    if (watch["enabled"]) {
      config.watch.enabled = watch["enabled"].as<bool>();
    }
    if (watch["interval_ms"]) {
      config.watch.intervalMs = parseNumeric<uint32_t>(watch["interval_ms"], path + ".watch.interval_ms");
    }
    if (watch["settle_ms"]) {
      config.watch.settleMs = parseNumeric<uint32_t>(watch["settle_ms"], path + ".watch.settle_ms");
    }
    if (config.watch.intervalMs < 100u || config.watch.intervalMs > 60000u) {
      fail(path + ".watch.interval_ms", "must be between 100 and 60000");
    }
    if (config.watch.settleMs > 60000u) {
      fail(path + ".watch.settle_ms", "must not exceed 60000");
    }
  }
  if (node["defaults"]) {
    const auto defaults = requireMap(node["defaults"], path + ".defaults");
    rejectUnknownKeys(defaults, path + ".defaults",
                      {"pv", "rpc", "admin_read", "admin_write"});
    if (defaults["pv"]) config.defaults.pv = parseAccessAssignment(defaults["pv"], path + ".defaults.pv");
    if (defaults["rpc"]) config.defaults.rpc = parseAccessAssignment(defaults["rpc"], path + ".defaults.rpc");
    if (defaults["admin_read"]) {
      config.defaults.adminRead = parseAccessAssignment(defaults["admin_read"], path + ".defaults.admin_read");
    }
    if (defaults["admin_write"]) {
      config.defaults.adminWrite = parseAccessAssignment(defaults["admin_write"], path + ".defaults.admin_write");
    }
  }

  if (!config.enabled) {
    if (!config.file.empty() || !config.macros.empty() || node["watch"] || node["defaults"]) {
      fail(path, "access settings require enabled: true");
    }
    return config;
  }

  if (config.file.empty()) {
    fail(path + ".file", "is required when access control is enabled");
  }
  std::filesystem::path policyPath(config.file);
  if (policyPath.is_relative()) {
    policyPath = configDirectory / policyPath;
  }
  config.file = std::filesystem::absolute(policyPath).lexically_normal().string();
  return config;
}

PrimitiveType parsePrimitiveType(const YAML::Node& node, const std::string& path) {
  const auto type = lowerCopy(parseString(node, path));
  if (type == "bool" || type == "boolean") return PrimitiveType::Boolean;
  if (type == "int8") return PrimitiveType::Int8;
  if (type == "uint8") return PrimitiveType::UInt8;
  if (type == "int16") return PrimitiveType::Int16;
  if (type == "uint16") return PrimitiveType::UInt16;
  if (type == "int32") return PrimitiveType::Int32;
  if (type == "uint32") return PrimitiveType::UInt32;
  if (type == "int64") return PrimitiveType::Int64;
  if (type == "uint64") return PrimitiveType::UInt64;
  if (type == "float32" || type == "float") return PrimitiveType::Float32;
  if (type == "float64" || type == "double") return PrimitiveType::Float64;
  if (type == "string") return PrimitiveType::String;
  fail(path, "unsupported primitive type '" + type + "'");
}

Shape parseShape(const YAML::Node& node, const std::string& path) {
  const auto shape = lowerCopy(parseString(node, path));
  if (shape == "scalar") return Shape::Scalar;
  if (shape == "array") return Shape::Array;
  fail(path, "unsupported shape '" + shape + "'");
}

DisplayForm parseDisplayForm(const YAML::Node& node, const std::string& path) {
  const auto form = lowerCopy(parseString(node, path));
  if (form == "default") return DisplayForm::Default;
  if (form == "string") return DisplayForm::String;
  if (form == "binary") return DisplayForm::Binary;
  if (form == "decimal") return DisplayForm::Decimal;
  if (form == "hex") return DisplayForm::Hex;
  if (form == "exponential") return DisplayForm::Exponential;
  if (form == "engineering") return DisplayForm::Engineering;
  fail(path, "unsupported display form '" + form + "'");
}

template <typename T>
TypedValue parseScalarInitial(const YAML::Node& node, const std::string& path) {
  if constexpr (std::is_same_v<T, std::string>) {
    return parseString(node, path);
  } else if constexpr (std::is_same_v<T, bool>) {
    return node.as<bool>();
  } else {
    return parseNumeric<T>(node, path);
  }
}

template <typename T>
TypedValue parseArrayInitial(const YAML::Node& node, const std::string& path) {
  requireSequence(node, path);
  std::vector<T> values;
  values.reserve(node.size());
  for (size_t index = 0; index < node.size(); ++index) {
    const auto elementPath = path + "[" + std::to_string(index) + "]";
    values.push_back(parseNumeric<T>(node[index], elementPath));
  }
  return values;
}

TypedValue parseInitialValue(const YAML::Node& node,
                             PrimitiveType type,
                             Shape shape,
                             const std::string& path) {
  if (!node) {
    return std::monostate{};
  }

  switch (shape) {
  case Shape::Scalar:
    switch (type) {
    case PrimitiveType::Boolean: return parseScalarInitial<bool>(node, path);
    case PrimitiveType::Int8: return parseScalarInitial<int8_t>(node, path);
    case PrimitiveType::UInt8: return parseScalarInitial<uint8_t>(node, path);
    case PrimitiveType::Int16: return parseScalarInitial<int16_t>(node, path);
    case PrimitiveType::UInt16: return parseScalarInitial<uint16_t>(node, path);
    case PrimitiveType::Int32: return parseScalarInitial<int32_t>(node, path);
    case PrimitiveType::UInt32: return parseScalarInitial<uint32_t>(node, path);
    case PrimitiveType::Int64: return parseScalarInitial<int64_t>(node, path);
    case PrimitiveType::UInt64: return parseScalarInitial<uint64_t>(node, path);
    case PrimitiveType::Float32: return parseScalarInitial<float>(node, path);
    case PrimitiveType::Float64: return parseScalarInitial<double>(node, path);
    case PrimitiveType::String: return parseScalarInitial<std::string>(node, path);
    }
    break;
  case Shape::Array:
    switch (type) {
    case PrimitiveType::Int8: return parseArrayInitial<int8_t>(node, path);
    case PrimitiveType::UInt8: return parseArrayInitial<uint8_t>(node, path);
    case PrimitiveType::Int16: return parseArrayInitial<int16_t>(node, path);
    case PrimitiveType::UInt16: return parseArrayInitial<uint16_t>(node, path);
    case PrimitiveType::Int32: return parseArrayInitial<int32_t>(node, path);
    case PrimitiveType::UInt32: return parseArrayInitial<uint32_t>(node, path);
    case PrimitiveType::Int64: return parseArrayInitial<int64_t>(node, path);
    case PrimitiveType::UInt64: return parseArrayInitial<uint64_t>(node, path);
    case PrimitiveType::Float32: return parseArrayInitial<float>(node, path);
    case PrimitiveType::Float64: return parseArrayInitial<double>(node, path);
    case PrimitiveType::Boolean:
    case PrimitiveType::String:
      fail(path, "array initial value is unsupported for this type");
    }
    break;
  }

  fail(path, "unsupported initial value");
}

RouteConfig parseRoute(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"backend", "key"});
  RouteConfig route;
  if (node["backend"]) {
    route.backend = parseString(node["backend"], path + ".backend");
  }
  route.key = parseString(requireNode(node, "key", path), path + ".key");
  if (route.key.empty()) {
    fail(path + ".key", "must not be empty");
  }
  return route;
}

ConfirmConfig parseConfirm(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"backend", "key", "timeout_ms"});
  ConfirmConfig confirm;
  if (node["backend"]) {
    confirm.backend = parseString(node["backend"], path + ".backend");
  }
  confirm.key = parseString(requireNode(node, "key", path), path + ".key");
  if (node["timeout_ms"]) {
    confirm.timeoutMs = parseNumeric<int>(node["timeout_ms"], path + ".timeout_ms");
  }
  if (confirm.key.empty()) {
    fail(path + ".key", "must not be empty");
  }
  if (confirm.timeoutMs <= 0 || confirm.timeoutMs > 300000) fail(path + ".timeout_ms", "must be 1..300000");
  return confirm;
}

RedisConfig parseRedisConfig(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"base_key", "host", "port", "user", "password", "workers", "readers"});
  const auto redisNode = node;

  RedisConfig config;
  config.baseKey = parseString(requireNode(redisNode, "base_key", path), path + ".base_key");
  config.host = parseString(requireNode(redisNode, "host", path), path + ".host");
  config.port = parseNumeric<uint16_t>(requireNode(redisNode, "port", path), path + ".port");
  if (redisNode["user"]) {
    config.user = parseString(redisNode["user"], path + ".user");
  }
  if (redisNode["password"]) {
    config.password = parseString(redisNode["password"], path + ".password");
  }
  if (redisNode["workers"]) {
    config.workers = parseNumeric<uint16_t>(redisNode["workers"], path + ".workers");
  }
  if (redisNode["readers"]) {
    config.readers = parseNumeric<uint16_t>(redisNode["readers"], path + ".readers");
  }

  if (config.host.empty()) fail(path + ".host", "must not be empty");
  if (!config.port) fail(path + ".port", "must be 1..65535");
  if (!config.workers || config.workers > 256) fail(path + ".workers", "must be 1..256");
  if (!config.readers || config.readers > 256) fail(path + ".readers", "must be 1..256");
  return config;
}

ChannelFinderConfig parseChannelFinderConfig(const YAML::Node& node, const std::string& path) {
  ChannelFinderConfig config;
  if (!node) {
    return config;
  }

  rejectUnknownKeys(node, path, {"url", "owner", "tags", "properties"});
  const auto channelFinderNode = node;
  if (channelFinderNode["url"]) {
    config.url = parseString(channelFinderNode["url"], path + ".url");
  }
  if (channelFinderNode["owner"]) {
    config.owner = parseString(channelFinderNode["owner"], path + ".owner");
    if (config.owner.empty()) {
      fail(path + ".owner", "must not be empty");
    }
  }
  if (channelFinderNode["tags"]) {
    const auto tags = requireSequence(channelFinderNode["tags"], path + ".tags");
    for (size_t index = 0; index < tags.size(); ++index) {
      const auto tag = parseString(tags[index], path + ".tags[" + std::to_string(index) + "]");
      if (tag.empty()) {
        fail(path + ".tags[" + std::to_string(index) + "]", "must not be empty");
      }
      config.tags.push_back(tag);
    }
  }
  if (channelFinderNode["properties"]) {
    const auto properties = requireMap(channelFinderNode["properties"], path + ".properties");
    for (const auto& entry : properties) {
      const auto name = parseString(entry.first, path + ".properties.<name>");
      if (name.empty()) {
        fail(path + ".properties", "property name must not be empty");
      }
      config.properties[name] = parseString(entry.second, path + ".properties." + name);
    }
  }

  return config;
}

void resolveBackendAlias(std::string& alias,
                         const std::string& path,
                         const RedisBackendConfigs& backends,
                         const bool legacySingleBackend) {
  if (alias.empty()) {
    if (backends.size() == 1u) {
      alias = backends.begin()->first;
      return;
    }
    fail(path, "must be set when multiple redis_backends are configured");
  }

  if (legacySingleBackend && alias == "redis" && backends.count(kDefaultRedisBackendAlias) == 1u) {
    alias = kDefaultRedisBackendAlias;
  }

  if (backends.count(alias) == 0u) {
    fail(path, "unknown redis backend '" + alias + "'");
  }
}

void parseLimitConfig(const YAML::Node& node, LimitConfig& config, const std::string& path, bool control = false) {
  if (!node) {
    return;
  }
  rejectUnknownKeys(node, path, control ? std::set<std::string>{"low", "high", "min_step"} : std::set<std::string>{"low", "high"});
  if (node["low"]) {
    config.low = parseNumeric<double>(node["low"], path + ".low");
  }
  if (node["high"]) {
    config.high = parseNumeric<double>(node["high"], path + ".high");
  }
  if (config.low && config.high && *config.low > *config.high) fail(path, "low must not exceed high");
}

MetadataConfig parseMetadata(const YAML::Node& node, const std::string& path) {
  MetadataConfig metadata;
  if (!node) {
    return metadata;
  }
  rejectUnknownKeys(node, path, {"description", "units", "precision", "form", "display", "control", "min_step"});
  if (node["description"]) {
    metadata.description = parseString(node["description"], path + ".description");
  }
  if (node["units"]) {
    metadata.units = parseString(node["units"], path + ".units");
  }
  if (node["precision"]) {
    metadata.precision = parseNumeric<int32_t>(node["precision"], path + ".precision");
  }
  if (node["form"]) {
    metadata.form = parseDisplayForm(node["form"], path + ".form");
  }
  parseLimitConfig(node["display"], metadata.display, path + ".display");
  parseLimitConfig(node["control"], metadata.control, path + ".control", true);
  if (node["min_step"]) {
    metadata.minStep = parseNumeric<double>(node["min_step"], path + ".min_step");
  }
  if (node["control"] && node["control"]["min_step"]) {
    metadata.minStep = parseNumeric<double>(node["control"]["min_step"], path + ".control.min_step");
  }
  if (metadata.minStep && *metadata.minStep < 0) fail(path + ".min_step", "must not be negative");
  return metadata;
}

AlarmConfig parseAlarmConfig(const YAML::Node& node, const std::string& path) {
  AlarmConfig alarm;
  if (!node) {
    return alarm;
  }
  rejectUnknownKeys(node, path, {"low_alarm", "low_warning", "high_warning", "high_alarm", "hysteresis"});
  if (node["low_alarm"]) {
    alarm.lowAlarm = parseNumeric<double>(node["low_alarm"], path + ".low_alarm");
  }
  if (node["low_warning"]) {
    alarm.lowWarning = parseNumeric<double>(node["low_warning"], path + ".low_warning");
  }
  if (node["high_warning"]) {
    alarm.highWarning = parseNumeric<double>(node["high_warning"], path + ".high_warning");
  }
  if (node["high_alarm"]) {
    alarm.highAlarm = parseNumeric<double>(node["high_alarm"], path + ".high_alarm");
  }
  if (node["hysteresis"]) {
    alarm.hysteresis = parseNumeric<double>(node["hysteresis"], path + ".hysteresis");
  }
  if (alarm.hysteresis < 0) fail(path + ".hysteresis", "must not be negative");
  std::optional<double> previous;
  for (const auto& threshold : {alarm.lowAlarm, alarm.lowWarning, alarm.highWarning, alarm.highAlarm}) {
    if (!threshold) continue;
    if (previous && *previous > *threshold) fail(path, "alarm thresholds must be ordered low_alarm <= low_warning <= high_warning <= high_alarm");
    previous = threshold;
  }
  return alarm;
}

std::optional<LinearTransformConfig> parseTransform(const YAML::Node& node, const std::string& path) {
  if (!node) {
    return std::nullopt;
  }
  rejectUnknownKeys(node, path, {"kind", "scale", "offset"});
  if (node["kind"]) {
    const auto kind = lowerCopy(parseString(node["kind"], path + ".kind"));
    if (kind != "linear") {
      fail(path + ".kind", "only linear transforms are supported");
    }
  }
  LinearTransformConfig transform;
  if (node["scale"]) {
    transform.scale = parseNumeric<double>(node["scale"], path + ".scale");
  }
  if (node["offset"]) {
    transform.offset = parseNumeric<double>(node["offset"], path + ".offset");
  }
  if (transform.scale == 0.0 || !std::isfinite(1.0 / transform.scale)) {
    fail(path + ".scale", "must be nonzero with a finite inverse");
  }
  return transform;
}

RpcServiceConfig parseRpcService(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"endpoint", "service", "suffix", "defaults", "access"});
  RpcServiceConfig svc;
  svc.endpoint = parseString(requireNode(node, "endpoint", path), path + ".endpoint");
  svc.service = parseString(requireNode(node, "service", path), path + ".service");
  if (node["suffix"]) svc.suffix = parseString(node["suffix"], path + ".suffix");
  if (node["defaults"]) {
    const auto& d = node["defaults"];
    requireMap(d, path + ".defaults");
    for (const auto& kv : d) {
      svc.defaults[kv.first.as<std::string>()] =
          parseString(kv.second, path + ".defaults." + kv.first.as<std::string>());
    }
  }
  if (node["access"]) svc.access = parseAccessAssignment(node["access"], path + ".access");
  if (svc.endpoint.empty()) fail(path + ".endpoint", "must not be empty");
  if (svc.service.empty()) fail(path + ".service", "must not be empty");
  return svc;
}

PVConfig parsePV(const YAML::Node& node, const std::string& path) {
  rejectUnknownKeys(node, path, {"name", "aliases", "type", "shape", "read", "write", "confirm", "metadata", "alarm", "transform", "initial", "access"});

  PVConfig pv;
  pv.name = parseString(requireNode(node, "name", path), path + ".name");
  if (pv.name.empty()) {
    fail(path + ".name", "must not be empty");
  }
  if (node["aliases"]) {
    const auto aliases = requireSequence(node["aliases"], path + ".aliases");
    if (aliases.size() == 0u) {
      fail(path + ".aliases", "must not be empty");
    }
    for (size_t index = 0; index < aliases.size(); ++index) {
      const auto aliasPath = path + ".aliases[" + std::to_string(index) + "]";
      const auto alias = parseString(aliases[index], aliasPath);
      if (alias.empty()) {
        fail(aliasPath, "must not be empty");
      }
      pv.aliases.push_back(alias);
    }
  }

  pv.type = parsePrimitiveType(requireNode(node, "type", path), path + ".type");
  pv.shape = parseShape(requireNode(node, "shape", path), path + ".shape");
  pv.read = parseRoute(requireNode(node, "read", path), path + ".read");

  if (node["write"]) {
    pv.write = parseRoute(node["write"], path + ".write");
  }
  if (node["confirm"]) {
    if (!pv.write) {
      fail(path + ".confirm", "requires a write route");
    }
    pv.confirm = parseConfirm(node["confirm"], path + ".confirm");
  }

  pv.metadata = parseMetadata(node["metadata"], path + ".metadata");
  pv.alarms = parseAlarmConfig(node["alarm"], path + ".alarm");
  pv.transform = parseTransform(node["transform"], path + ".transform");
  pv.initialValue = parseInitialValue(node["initial"], pv.type, pv.shape, path + ".initial");
  if (node["access"]) pv.access = parseAccessAssignment(node["access"], path + ".access");

  if (pv.shape == Shape::Array && !isArrayElementTypeSupported(pv.type)) {
    fail(path + ".type", "this array element type is unsupported");
  }
  if (pv.transform.has_value() && !isFloatingPointType(pv.type)) {
    fail(path + ".transform", "linear transforms are only supported for floating-point PVs");
  }
  if (pv.shape == Shape::Array && (pv.alarms.lowAlarm || pv.alarms.lowWarning || pv.alarms.highWarning || pv.alarms.highAlarm)) {
    fail(path + ".alarm", "array threshold alarms are unsupported");
  }
  if (!isNumericType(pv.type)) {
    if (pv.metadata.display.low || pv.metadata.display.high || pv.metadata.control.low || pv.metadata.control.high || pv.metadata.minStep) {
      fail(path + ".metadata", "display/control numeric limits require a numeric type");
    }
    if (pv.alarms.lowAlarm || pv.alarms.lowWarning || pv.alarms.highWarning || pv.alarms.highAlarm) {
      fail(path + ".alarm", "threshold alarms require a numeric type");
    }
  }

  return pv;
}

void validateTopLevelSchema(const YAML::Node& root) {
  if (root["PVList"] || root["PVBase"] || root["RedisBase"]) {
    fail("root", "old flat prototype schema is not supported");
  }
}

DiscoveryConfig parseDiscovery(const YAML::Node& node) {
  DiscoveryConfig value;
  if (!node) return value;
  rejectUnknownKeys(node, "root.discovery", {"enabled", "bind_address", "udp_port", "timeout_ms", "max_holdoff_ms", "max_records", "max_bytes"});
  std::set<std::string> keys;
  for (const auto& entry : node) {
    const auto key = entry.first.as<std::string>();
    if (!keys.insert(key).second) fail("root.discovery." + key, "duplicate key");
  }
  if (node["enabled"]) value.enabled = parseNumeric<bool>(node["enabled"], "root.discovery.enabled");
  if (node["bind_address"]) value.bindAddress = parseString(node["bind_address"], "root.discovery.bind_address");
  in_addr address{};
  if (inet_pton(AF_INET, value.bindAddress.c_str(), &address) != 1)
    fail("root.discovery.bind_address", "expected an IPv4 address");
  const auto bounded = [&](const char* key, uint64_t initial, uint64_t low, uint64_t high) {
    const auto result = node[key] ? parseNumeric<uint64_t>(node[key], "root.discovery." + std::string(key)) : initial;
    if (result < low || result > high) fail("root.discovery." + std::string(key), "outside supported range");
    return result;
  };
  value.udpPort = bounded("udp_port", value.udpPort, 0, 65535);
  value.timeoutMs = bounded("timeout_ms", value.timeoutMs, 1, 300000);
  value.maxHoldoffMs = bounded("max_holdoff_ms", value.maxHoldoffMs, 0, 60000);
  value.maxRecords = bounded("max_records", value.maxRecords, 1, 1000000);
  value.maxBytes = bounded("max_bytes", value.maxBytes, 1024, 1024ull * 1024 * 1024);
  return value;
}

AppConfig parseConfig(const YAML::Node& root, const std::filesystem::path& configDirectory) {
  requireMap(root, "root");
  validateTopLevelSchema(root);
  std::vector<YAML::Node> ancestors;
  size_t visited = 0;
  validateTree(root, "root", ancestors, visited);
  rejectUnknownKeys(root, "root", {"schema_version", "server", "access", "redis", "redis_backends", "alarms", "channelfinder", "discovery", "pvs", "rpc_services"});

  AppConfig config;
  config.legacyInput = !root["schema_version"];
  if (!config.legacyInput) config.schemaVersion = parseNumeric<uint32_t>(root["schema_version"], "root.schema_version");
  if (config.schemaVersion != 1) fail("root.schema_version", "only schema version 1 is supported");
  config.discovery = parseDiscovery(root["discovery"]);

  config.access = parseAccessConfig(root["access"], configDirectory, "root.access");

  const auto serverNode = requireMap(requireNode(root, "server", "root"), "root.server");
  rejectUnknownKeys(serverNode, "root.server", {"instance", "namespace", "interfaces", "tcp_port", "udp_port", "auto_beacon"});
  config.server.instance = parseString(requireNode(serverNode, "instance", "root.server"), "root.server.instance");
  if (serverNode["namespace"]) {
    config.server.nameSpace = parseString(serverNode["namespace"], "root.server.namespace");
  }
  if (serverNode["interfaces"]) {
    const auto interfaces = requireSequence(serverNode["interfaces"], "root.server.interfaces");
    for (size_t index = 0; index < interfaces.size(); ++index) {
      config.server.interfaces.push_back(parseString(interfaces[index], "root.server.interfaces[" + std::to_string(index) + "]"));
    }
  }
  if (serverNode["tcp_port"]) {
    config.server.tcpPort = parseNumeric<unsigned short>(serverNode["tcp_port"], "root.server.tcp_port");
  }
  if (serverNode["udp_port"]) {
    config.server.udpPort = parseNumeric<unsigned short>(serverNode["udp_port"], "root.server.udp_port");
  }
  if (serverNode["auto_beacon"]) {
    config.server.autoBeacon = serverNode["auto_beacon"].as<bool>();
  }

  if (config.server.instance.empty()) fail("root.server.instance", "must not be empty");
  config.channelFinder = parseChannelFinderConfig(root["channelfinder"], "root.channelfinder");

  const bool hasLegacyRedis = static_cast<bool>(root["redis"]);
  const bool hasRedisBackends = static_cast<bool>(root["redis_backends"]);
  if (hasLegacyRedis == hasRedisBackends) {
    fail("root", "specify exactly one of 'redis' or 'redis_backends'");
  }

  if (hasLegacyRedis) {
    config.redisBackends.emplace(kDefaultRedisBackendAlias, parseRedisConfig(requireNode(root, "redis", "root"), "root.redis"));
  } else {
    const auto redisBackendsNode = requireMap(requireNode(root, "redis_backends", "root"), "root.redis_backends");
    if (redisBackendsNode.size() == 0u) {
      fail("root.redis_backends", "must not be empty");
    }
    for (const auto& entry : redisBackendsNode) {
      const auto alias = parseString(entry.first, "root.redis_backends.<alias>");
      if (alias.empty()) {
        fail("root.redis_backends", "backend alias must not be empty");
      }
      const auto [it, inserted] = config.redisBackends.emplace(alias, parseRedisConfig(entry.second, "root.redis_backends." + alias));
      if (!inserted) {
        fail("root.redis_backends." + alias, "duplicate redis backend alias '" + alias + "'");
      }
    }
  }

  if (root["alarms"]) {
    rejectUnknownKeys(root["alarms"], "root.alarms", {"backend", "stream"});
    const auto alarmNode = root["alarms"];
    if (alarmNode["backend"]) {
      config.alarms.backend = parseString(alarmNode["backend"], "root.alarms.backend");
    }
    if (alarmNode["stream"]) {
      config.alarms.stream = parseString(alarmNode["stream"], "root.alarms.stream");
    }
  }
  if (config.alarms.stream.empty()) fail("root.alarms.stream", "must not be empty");
  resolveBackendAlias(config.alarms.backend, "root.alarms.backend", config.redisBackends, hasLegacyRedis);

  // `pvs` is optional: an IOC may expose only RPC services (rpc_services) and no
  // Redis-backed PVs.
  const auto pvsNode = root["pvs"] ? requireSequence(root["pvs"], "root.pvs") : YAML::Node();

  std::set<std::string> servedNames;
  std::set<std::pair<std::string, std::string>> subscribedKeys;
  for (size_t index = 0; index < pvsNode.size(); ++index) {
    auto pv = parsePV(pvsNode[index], "root.pvs[" + std::to_string(index) + "]");

    if (pv.access && !config.access.enabled) {
      fail("root.pvs[" + std::to_string(index) + "].access", "requires root.access.enabled: true");
    }
    if (!pv.access && config.access.enabled) {
      pv.access = config.access.defaults.pv;
    }

    resolveBackendAlias(pv.read.backend,
                        "root.pvs[" + std::to_string(index) + "].read.backend",
                        config.redisBackends,
                        hasLegacyRedis);
    if (pv.write) {
      resolveBackendAlias(pv.write->backend,
                          "root.pvs[" + std::to_string(index) + "].write.backend",
                          config.redisBackends,
                          hasLegacyRedis);
    }
    if (pv.confirm) {
      resolveBackendAlias(pv.confirm->backend,
                          "root.pvs[" + std::to_string(index) + "].confirm.backend",
                          config.redisBackends,
                          hasLegacyRedis);
    }
    const auto reservedNames = adminPVNames(config.server);
    const auto names = fullPVNames(config.server, pv);
    for (size_t nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
      const auto path = nameIndex == 0u
          ? "root.pvs[" + std::to_string(index) + "].name"
          : "root.pvs[" + std::to_string(index) + "].aliases[" +
                std::to_string(nameIndex - 1u) + "]";
      const auto& name = names[nameIndex];
      if (name.find('\0') != std::string::npos) fail(path, "PV names must not contain NUL");
      if (!servedNames.insert(name).second) {
        fail(path, "duplicate served PV name '" + name + "'");
      }
      for (const auto& reservedName : reservedNames) {
        if (name == reservedName) {
          fail(path, "PV name conflicts with reserved metadata PV '" + reservedName + "'");
        }
      }
    }
    if (!subscribedKeys.insert({pv.read.backend, pv.read.key}).second) {
      fail("root.pvs[" + std::to_string(index) + "].read.key",
           "duplicate subscribed key '" + pv.read.backend + ":" + pv.read.key + "'");
    }
    if (pv.confirm && (pv.confirm->backend != pv.read.backend || pv.confirm->key != pv.read.key)) {
      if (!subscribedKeys.insert({pv.confirm->backend, pv.confirm->key}).second) {
        fail("root.pvs[" + std::to_string(index) + "].confirm.key",
             "duplicate subscribed key '" + pv.confirm->backend + ":" + pv.confirm->key + "'");
      }
    }
    config.pvs.push_back(std::move(pv));
  }

  // Generic gRPC services exposed as RPC PVs (one PV per reflected method).
  if (root["rpc_services"]) {
    const auto svcNode = requireSequence(root["rpc_services"], "root.rpc_services");
    for (size_t i = 0; i < svcNode.size(); ++i) {
      auto service = parseRpcService(svcNode[i], "root.rpc_services[" + std::to_string(i) + "]");
      if (service.access && !config.access.enabled) {
        fail("root.rpc_services[" + std::to_string(i) + "].access", "requires root.access.enabled: true");
      }
      if (!service.access && config.access.enabled) {
        service.access = config.access.defaults.rpc;
      }
      config.rpcServices.push_back(std::move(service));
    }
  }

  if (config.pvs.empty() && config.rpcServices.empty()) {
    fail("root", "must define at least one of 'pvs' or 'rpc_services'");
  }

  return config;
}

}  // namespace

AppConfig loadConfigFile(const std::string& path) {
  const auto absolutePath = std::filesystem::absolute(path);
  return parseConfig(YAML::LoadFile(path), absolutePath.parent_path());
}

AppConfig loadConfigString(const std::string& text) {
  return parseConfig(YAML::Load(text), std::filesystem::current_path());
}

std::string summarizeConfig(const AppConfig& config) {
  std::ostringstream stream;
  stream << "instance=" << config.server.instance
         << " namespace=" << (config.server.nameSpace.empty() ? "<none>" : config.server.nameSpace)
         << " redis_backends=" << config.redisBackends.size()
         << " pvs=" << config.pvs.size()
         << " rpc_services=" << config.rpcServices.size()
         << " access=" << (config.access.enabled ? "enabled" : "disabled");
  if (config.access.enabled) {
    stream << "\naccess_file=" << config.access.file
           << " watch=" << (config.access.watch.enabled ? "enabled" : "disabled")
           << " macros=" << config.access.macros.size();
  }
  for (const auto& entry : config.redisBackends) {
    stream << "\nbackend[" << entry.first << "]="
           << entry.second.host << ":" << entry.second.port
           << " base_key=" << entry.second.baseKey;
  }
  for (const auto& svc : config.rpcServices) {
    stream << "\n- rpc_service " << svc.service << " @ " << svc.endpoint
           << " -> " << (config.server.nameSpace.empty() ? "" : config.server.nameSpace + ":")
           << "<METHOD>" << svc.suffix;
  }
  for (const auto& pv : config.pvs) {
    stream << "\n- " << fullPVName(config.server, pv)
           << " [" << toString(pv.shape) << " " << toString(pv.type) << "]"
           << " read=" << pv.read.backend << ":" << pv.read.key;
    if (pv.write) {
      stream << " write=" << pv.write->backend << ":" << pv.write->key;
    }
    if (pv.confirm) {
      stream << " confirm=" << pv.confirm->backend << ":" << pv.confirm->key;
    }
    if (!pv.aliases.empty()) {
      stream << " aliases=";
      for (size_t index = 0; index < pv.aliases.size(); ++index) {
        if (index != 0u) {
          stream << ",";
        }
        stream << pv.aliases[index];
      }
    }
    if (pv.access) {
      stream << " access=" << pv.access->asg << ":ASL" << pv.access->asl;
    }
  }
  return stream.str();
}

bool isNumericType(const PrimitiveType type) {
  return type != PrimitiveType::Boolean && type != PrimitiveType::String;
}

bool isFloatingPointType(const PrimitiveType type) {
  return type == PrimitiveType::Float32 || type == PrimitiveType::Float64;
}

bool isArrayElementTypeSupported(const PrimitiveType type) {
  return type != PrimitiveType::Boolean && type != PrimitiveType::String;
}

std::string toString(const PrimitiveType type) {
  switch (type) {
  case PrimitiveType::Boolean: return "bool";
  case PrimitiveType::Int8: return "int8";
  case PrimitiveType::UInt8: return "uint8";
  case PrimitiveType::Int16: return "int16";
  case PrimitiveType::UInt16: return "uint16";
  case PrimitiveType::Int32: return "int32";
  case PrimitiveType::UInt32: return "uint32";
  case PrimitiveType::Int64: return "int64";
  case PrimitiveType::UInt64: return "uint64";
  case PrimitiveType::Float32: return "float32";
  case PrimitiveType::Float64: return "float64";
  case PrimitiveType::String: return "string";
  }
  return "unknown";
}

std::string toString(const Shape shape) {
  switch (shape) {
  case Shape::Scalar: return "scalar";
  case Shape::Array: return "array";
  }
  return "unknown";
}

std::string toString(const DisplayForm form) {
  switch (form) {
  case DisplayForm::Default: return "default";
  case DisplayForm::String: return "string";
  case DisplayForm::Binary: return "binary";
  case DisplayForm::Decimal: return "decimal";
  case DisplayForm::Hex: return "hex";
  case DisplayForm::Exponential: return "exponential";
  case DisplayForm::Engineering: return "engineering";
  }
  return "unknown";
}

bool sameReaderTopology(const PVConfig& lhs, const PVConfig& rhs) {
  const auto sameOptionalRoute = [](const auto& left, const auto& right) {
    if (left.has_value() != right.has_value()) {
      return false;
    }
    if (!left) {
      return true;
    }
    return left->backend == right->backend && left->key == right->key;
  };

  const bool sameConfirm =
      (!lhs.confirm && !rhs.confirm) ||
      (lhs.confirm && rhs.confirm &&
       lhs.confirm->backend == rhs.confirm->backend &&
       lhs.confirm->key == rhs.confirm->key &&
       lhs.confirm->timeoutMs == rhs.confirm->timeoutMs);

  return lhs.name == rhs.name &&
         lhs.type == rhs.type &&
         lhs.shape == rhs.shape &&
         lhs.read.backend == rhs.read.backend &&
         lhs.read.key == rhs.read.key &&
         sameOptionalRoute(lhs.write, rhs.write) &&
         sameConfirm;
}

bool sameServerConfig(const ServerConfig& lhs, const ServerConfig& rhs) {
  return lhs.instance == rhs.instance &&
         lhs.nameSpace == rhs.nameSpace &&
         lhs.interfaces == rhs.interfaces &&
         lhs.tcpPort == rhs.tcpPort &&
         lhs.udpPort == rhs.udpPort &&
         lhs.autoBeacon == rhs.autoBeacon;
}

bool sameAccessAssignment(const AccessAssignment& lhs, const AccessAssignment& rhs) {
  return lhs.asg == rhs.asg && lhs.asl == rhs.asl;
}

bool sameAccessConfig(const AccessConfig& lhs, const AccessConfig& rhs) {
  return lhs.enabled == rhs.enabled &&
         lhs.file == rhs.file &&
         lhs.macros == rhs.macros &&
         lhs.watch.enabled == rhs.watch.enabled &&
         lhs.watch.intervalMs == rhs.watch.intervalMs &&
         lhs.watch.settleMs == rhs.watch.settleMs &&
         sameAccessAssignment(lhs.defaults.pv, rhs.defaults.pv) &&
         sameAccessAssignment(lhs.defaults.rpc, rhs.defaults.rpc) &&
         sameAccessAssignment(lhs.defaults.adminRead, rhs.defaults.adminRead) &&
         sameAccessAssignment(lhs.defaults.adminWrite, rhs.defaults.adminWrite);
}

bool sameRedisConfig(const RedisConfig& lhs, const RedisConfig& rhs) {
  return lhs.baseKey == rhs.baseKey &&
         lhs.host == rhs.host &&
         lhs.port == rhs.port &&
         lhs.user == rhs.user &&
         lhs.password == rhs.password &&
         lhs.workers == rhs.workers &&
         lhs.readers == rhs.readers;
}

bool sameRedisBackends(const RedisBackendConfigs& lhs, const RedisBackendConfigs& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (const auto& entry : lhs) {
    const auto other = rhs.find(entry.first);
    if (other == rhs.end() || !sameRedisConfig(entry.second, other->second)) {
      return false;
    }
  }

  return true;
}

bool sameAlarmStreamConfig(const AlarmStreamConfig& lhs, const AlarmStreamConfig& rhs) {
  return lhs.backend == rhs.backend &&
         lhs.stream == rhs.stream;
}

std::string fullPVName(const ServerConfig& server, const PVConfig& pv) {
  if (server.nameSpace.empty()) {
    return pv.name;
  }
  return server.nameSpace + ":" + pv.name;
}

std::vector<std::string> fullPVNames(const ServerConfig& server, const PVConfig& pv) {
  std::vector<std::string> names;
  names.reserve(1u + pv.aliases.size());
  names.push_back(fullPVName(server, pv));
  names.insert(names.end(), pv.aliases.begin(), pv.aliases.end());
  return names;
}

std::string adminPVName(const ServerConfig& server, const std::string& suffix) {
  return "SYS:" + server.instance + ":" + suffix;
}

std::string versionPVName(const ServerConfig& server) {
  return server.instance + ":version";
}

std::string revisionPVName(const ServerConfig& server) {
  return server.instance + ":revision";
}

std::vector<std::string> adminPVNames(const ServerConfig& server) {
  return {
    versionPVName(server),
    revisionPVName(server),
    adminPVName(server, "version"),
    adminPVName(server, "revision"),
    adminPVName(server, "config:reload"),
    adminPVName(server, "config:generation"),
    adminPVName(server, "config:lastStatus"),
    adminPVName(server, "config:lastError"),
    adminPVName(server, "config:lastDiff"),
    adminPVName(server, "stats:pvCount"),
    adminPVName(server, "backend:health"),
    adminPVName(server, "access:reload"),
    adminPVName(server, "access:enabled"),
    adminPVName(server, "access:generation"),
    adminPVName(server, "access:lastStatus"),
    adminPVName(server, "access:lastError"),
    adminPVName(server, "access:policyFingerprint"),
    adminPVName(server, "access:watchStatus"),
    adminPVName(server, "access:activeClients"),
    adminPVName(server, "access:deniedReads"),
    adminPVName(server, "access:deniedWrites"),
    adminPVName(server, "access:rightsChanges"),
    adminPVName(server, "discovery:status"),
  };
}

bool sameDiscoveryConfig(const DiscoveryConfig& lhs, const DiscoveryConfig& rhs) {
  return std::tie(lhs.enabled, lhs.bindAddress, lhs.udpPort, lhs.timeoutMs, lhs.maxHoldoffMs, lhs.maxRecords, lhs.maxBytes) ==
         std::tie(rhs.enabled, rhs.bindAddress, rhs.udpPort, rhs.timeoutMs, rhs.maxHoldoffMs, rhs.maxRecords, rhs.maxBytes);
}

}  // namespace redis_pvxs_ioc
