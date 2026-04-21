#include "redis_pvxs_ioc/config.h"

#include <algorithm>
#include <cctype>
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

template <typename T>
T parseNumeric(const YAML::Node& node, const std::string& path) {
  try {
    return node.as<T>();
  } catch (const std::exception& ex) {
    fail(path, ex.what());
  }
}

template <>
int8_t parseNumeric<int8_t>(const YAML::Node& node, const std::string& path) {
  return static_cast<int8_t>(parseNumeric<int>(node, path));
}

template <>
uint8_t parseNumeric<uint8_t>(const YAML::Node& node, const std::string& path) {
  return static_cast<uint8_t>(parseNumeric<unsigned int>(node, path));
}

std::string parseString(const YAML::Node& node, const std::string& path) {
  try {
    return node.as<std::string>();
  } catch (const std::exception& ex) {
    fail(path, ex.what());
  }
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
  requireMap(node, path);
  RouteConfig route;
  if (node["backend"]) {
    route.backend = lowerCopy(parseString(node["backend"], path + ".backend"));
  }
  if (route.backend != "redis") {
    fail(path, "only redis backends are supported in the MVP");
  }
  route.key = parseString(requireNode(node, "key", path), path + ".key");
  if (route.key.empty()) {
    fail(path + ".key", "must not be empty");
  }
  return route;
}

ConfirmConfig parseConfirm(const YAML::Node& node, const std::string& path) {
  requireMap(node, path);
  ConfirmConfig confirm;
  if (node["backend"]) {
    confirm.backend = lowerCopy(parseString(node["backend"], path + ".backend"));
  }
  if (confirm.backend != "redis") {
    fail(path, "only redis backends are supported in the MVP");
  }
  confirm.key = parseString(requireNode(node, "key", path), path + ".key");
  if (node["timeout_ms"]) {
    confirm.timeoutMs = parseNumeric<int>(node["timeout_ms"], path + ".timeout_ms");
  }
  if (confirm.key.empty()) {
    fail(path + ".key", "must not be empty");
  }
  return confirm;
}

void parseLimitConfig(const YAML::Node& node, LimitConfig& config, const std::string& path) {
  if (!node) {
    return;
  }
  requireMap(node, path);
  if (node["low"]) {
    config.low = parseNumeric<double>(node["low"], path + ".low");
  }
  if (node["high"]) {
    config.high = parseNumeric<double>(node["high"], path + ".high");
  }
}

MetadataConfig parseMetadata(const YAML::Node& node, const std::string& path) {
  MetadataConfig metadata;
  if (!node) {
    return metadata;
  }
  requireMap(node, path);
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
  parseLimitConfig(node["control"], metadata.control, path + ".control");
  if (node["min_step"]) {
    metadata.minStep = parseNumeric<double>(node["min_step"], path + ".min_step");
  }
  if (node["control"] && node["control"]["min_step"]) {
    metadata.minStep = parseNumeric<double>(node["control"]["min_step"], path + ".control.min_step");
  }
  return metadata;
}

AlarmConfig parseAlarmConfig(const YAML::Node& node, const std::string& path) {
  AlarmConfig alarm;
  if (!node) {
    return alarm;
  }
  requireMap(node, path);
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
  return alarm;
}

std::optional<LinearTransformConfig> parseTransform(const YAML::Node& node, const std::string& path) {
  if (!node) {
    return std::nullopt;
  }
  requireMap(node, path);
  if (node["kind"]) {
    const auto kind = lowerCopy(parseString(node["kind"], path + ".kind"));
    if (kind != "linear") {
      fail(path + ".kind", "only linear transforms are supported in the MVP");
    }
  }
  LinearTransformConfig transform;
  if (node["scale"]) {
    transform.scale = parseNumeric<double>(node["scale"], path + ".scale");
  }
  if (node["offset"]) {
    transform.offset = parseNumeric<double>(node["offset"], path + ".offset");
  }
  if (transform.scale == 0.0) {
    fail(path + ".scale", "must not be zero");
  }
  return transform;
}

PVConfig parsePV(const YAML::Node& node, const std::string& path) {
  requireMap(node, path);

  PVConfig pv;
  pv.name = parseString(requireNode(node, "name", path), path + ".name");
  if (pv.name.empty()) {
    fail(path + ".name", "must not be empty");
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

  if (pv.shape == Shape::Array && !isArrayElementTypeSupported(pv.type)) {
    fail(path + ".type", "this array element type is unsupported in the MVP");
  }
  if (pv.transform.has_value() && !isFloatingPointType(pv.type)) {
    fail(path + ".transform", "linear transforms are only supported for floating-point PVs");
  }
  if (pv.shape == Shape::Array && (pv.alarms.lowAlarm || pv.alarms.lowWarning || pv.alarms.highWarning || pv.alarms.highAlarm)) {
    fail(path + ".alarm", "array threshold alarms are out of scope for the MVP");
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

AppConfig parseConfig(const YAML::Node& root) {
  requireMap(root, "root");
  validateTopLevelSchema(root);

  AppConfig config;

  const auto serverNode = requireMap(requireNode(root, "server", "root"), "root.server");
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

  const auto redisNode = requireMap(requireNode(root, "redis", "root"), "root.redis");
  config.redis.baseKey = parseString(requireNode(redisNode, "base_key", "root.redis"), "root.redis.base_key");
  config.redis.host = parseString(requireNode(redisNode, "host", "root.redis"), "root.redis.host");
  config.redis.port = parseNumeric<uint16_t>(requireNode(redisNode, "port", "root.redis"), "root.redis.port");
  if (redisNode["user"]) {
    config.redis.user = parseString(redisNode["user"], "root.redis.user");
  }
  if (redisNode["password"]) {
    config.redis.password = parseString(redisNode["password"], "root.redis.password");
  }
  if (redisNode["workers"]) {
    config.redis.workers = parseNumeric<uint16_t>(redisNode["workers"], "root.redis.workers");
  }
  if (redisNode["readers"]) {
    config.redis.readers = parseNumeric<uint16_t>(redisNode["readers"], "root.redis.readers");
  }

  if (root["alarms"]) {
    const auto alarmNode = requireMap(root["alarms"], "root.alarms");
    if (alarmNode["stream"]) {
      config.alarms.stream = parseString(alarmNode["stream"], "root.alarms.stream");
    }
  }

  const auto pvsNode = requireSequence(requireNode(root, "pvs", "root"), "root.pvs");
  if (pvsNode.size() == 0) {
    fail("root.pvs", "must not be empty");
  }

  std::set<std::string> pvNames;
  std::set<std::string> subscribedKeys;
  for (size_t index = 0; index < pvsNode.size(); ++index) {
    auto pv = parsePV(pvsNode[index], "root.pvs[" + std::to_string(index) + "]");
    if (!pvNames.insert(pv.name).second) {
      fail("root.pvs[" + std::to_string(index) + "].name", "duplicate PV name '" + pv.name + "'");
    }
    if (!subscribedKeys.insert(pv.read.key).second) {
      fail("root.pvs[" + std::to_string(index) + "].read.key", "duplicate subscribed key '" + pv.read.key + "'");
    }
    if (pv.confirm && pv.confirm->key != pv.read.key) {
      if (!subscribedKeys.insert(pv.confirm->key).second) {
        fail("root.pvs[" + std::to_string(index) + "].confirm.key",
             "duplicate subscribed key '" + pv.confirm->key + "'");
      }
    }
    config.pvs.push_back(std::move(pv));
  }

  return config;
}

}  // namespace

AppConfig loadConfigFile(const std::string& path) {
  return parseConfig(YAML::LoadFile(path));
}

AppConfig loadConfigString(const std::string& text) {
  return parseConfig(YAML::Load(text));
}

std::string summarizeConfig(const AppConfig& config) {
  std::ostringstream stream;
  stream << "instance=" << config.server.instance
         << " namespace=" << (config.server.nameSpace.empty() ? "<none>" : config.server.nameSpace)
         << " redis=" << config.redis.host << ":" << config.redis.port
         << " base_key=" << config.redis.baseKey
         << " pvs=" << config.pvs.size();
  for (const auto& pv : config.pvs) {
    stream << "\n- " << fullPVName(config.server, pv)
           << " [" << toString(pv.shape) << " " << toString(pv.type) << "]"
           << " read=" << pv.read.key;
    if (pv.write) {
      stream << " write=" << pv.write->key;
    }
    if (pv.confirm) {
      stream << " confirm=" << pv.confirm->key;
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

bool sameRedisConfig(const RedisConfig& lhs, const RedisConfig& rhs) {
  return lhs.baseKey == rhs.baseKey &&
         lhs.host == rhs.host &&
         lhs.port == rhs.port &&
         lhs.user == rhs.user &&
         lhs.password == rhs.password &&
         lhs.workers == rhs.workers &&
         lhs.readers == rhs.readers;
}

bool sameAlarmStreamConfig(const AlarmStreamConfig& lhs, const AlarmStreamConfig& rhs) {
  return lhs.stream == rhs.stream;
}

std::string fullPVName(const ServerConfig& server, const PVConfig& pv) {
  if (server.nameSpace.empty()) {
    return pv.name;
  }
  return server.nameSpace + ":" + pv.name;
}

std::string adminPVName(const ServerConfig& server, const std::string& suffix) {
  return "SYS:" + server.instance + ":" + suffix;
}

}  // namespace redis_pvxs_ioc
