#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace redis_pvxs_ioc {

inline constexpr const char kDefaultRedisBackendAlias[] = "default";

enum class PrimitiveType {
  Boolean,
  Int8,
  UInt8,
  Int16,
  UInt16,
  Int32,
  UInt32,
  Int64,
  UInt64,
  Float32,
  Float64,
  String,
};

enum class Shape {
  Scalar,
  Array,
};

enum class DisplayForm {
  Default,
  String,
  Binary,
  Decimal,
  Hex,
  Exponential,
  Engineering,
};

struct RouteConfig {
  std::string backend;
  std::string key;
};

struct ConfirmConfig {
  std::string backend;
  std::string key;
  int timeoutMs = 250;
};

struct LimitConfig {
  std::optional<double> low;
  std::optional<double> high;
};

struct AlarmConfig {
  std::optional<double> lowAlarm;
  std::optional<double> lowWarning;
  std::optional<double> highWarning;
  std::optional<double> highAlarm;
  double hysteresis = 0.0;
};

struct MetadataConfig {
  std::string description;
  std::string units;
  std::optional<int32_t> precision;
  std::optional<DisplayForm> form;
  LimitConfig display;
  LimitConfig control;
  std::optional<double> minStep;
};

struct LinearTransformConfig {
  double scale = 1.0;
  double offset = 0.0;
};

// A backend gRPC service to expose as RPC PVs. The IOC reflects the service at
// startup and creates one RPC PV per method; it has NO compiled-in knowledge of
// the service's methods or message types (see docs/rpc-forwarding.md). Each PV
// is named  <server.namespace>:<UPPER_SNAKE(MethodName)><suffix>.
struct RpcServiceConfig {
  std::string endpoint;   // gRPC "host:port"
  std::string service;    // fully-qualified, e.g. "bpm.query.v1.BpmQuery"
  std::string suffix;     // appended to each derived PV name (e.g. "_RPC")
  // Fixed request-field defaults applied (by proto field name / dotted path /
  // unique leaf) before per-call pvxcall args. Fields that a given method's
  // request doesn't have are ignored, so one map can serve every method.
  std::map<std::string, std::string> defaults;
};

using TypedValue = std::variant<
  std::monostate,
  bool,
  int8_t,
  uint8_t,
  int16_t,
  uint16_t,
  int32_t,
  uint32_t,
  int64_t,
  uint64_t,
  float,
  double,
  std::string,
  std::vector<int8_t>,
  std::vector<uint8_t>,
  std::vector<int16_t>,
  std::vector<uint16_t>,
  std::vector<int32_t>,
  std::vector<uint32_t>,
  std::vector<int64_t>,
  std::vector<uint64_t>,
  std::vector<float>,
  std::vector<double>>;

struct PVConfig {
  std::string name;
  PrimitiveType type = PrimitiveType::Float64;
  Shape shape = Shape::Scalar;
  RouteConfig read;
  std::optional<RouteConfig> write;
  std::optional<ConfirmConfig> confirm;
  MetadataConfig metadata;
  AlarmConfig alarms;
  std::optional<LinearTransformConfig> transform;
  TypedValue initialValue;
};

struct ServerConfig {
  std::string instance;
  std::string nameSpace;
  std::vector<std::string> interfaces;
  std::optional<unsigned short> tcpPort;
  std::optional<unsigned short> udpPort;
  bool autoBeacon = true;
};

struct RedisConfig {
  std::string baseKey;
  std::string host = "127.0.0.1";
  uint16_t port = 6379;
  std::string user;
  std::string password;
  uint16_t workers = 1;
  uint16_t readers = 1;
};

using RedisBackendConfigs = std::map<std::string, RedisConfig>;

struct AlarmStreamConfig {
  std::string backend;
  std::string stream = "acorn:alarms";
};

struct ChannelFinderConfig {
  std::string url;
  std::string owner = "redis-pvxs-ioc";
  std::vector<std::string> tags;
  std::map<std::string, std::string> properties;
};

struct AppConfig {
  ServerConfig server;
  RedisBackendConfigs redisBackends;
  AlarmStreamConfig alarms;
  ChannelFinderConfig channelFinder;
  std::vector<PVConfig> pvs;
  std::vector<RpcServiceConfig> rpcServices;
};

AppConfig loadConfigFile(const std::string& path);
AppConfig loadConfigString(const std::string& text);
std::string summarizeConfig(const AppConfig& config);

bool isNumericType(PrimitiveType type);
bool isFloatingPointType(PrimitiveType type);
bool isArrayElementTypeSupported(PrimitiveType type);

std::string toString(PrimitiveType type);
std::string toString(Shape shape);
std::string toString(DisplayForm form);

bool sameReaderTopology(const PVConfig& lhs, const PVConfig& rhs);
bool sameServerConfig(const ServerConfig& lhs, const ServerConfig& rhs);
bool sameRedisConfig(const RedisConfig& lhs, const RedisConfig& rhs);
bool sameRedisBackends(const RedisBackendConfigs& lhs, const RedisBackendConfigs& rhs);
bool sameAlarmStreamConfig(const AlarmStreamConfig& lhs, const AlarmStreamConfig& rhs);

std::string fullPVName(const ServerConfig& server, const PVConfig& pv);
std::string adminPVName(const ServerConfig& server, const std::string& suffix);

}  // namespace redis_pvxs_ioc
