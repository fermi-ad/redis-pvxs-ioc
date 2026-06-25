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

// One of the four BpmQuery gRPC RPCs forwarded by a `pvxcall` to this PV.
enum class RpcMethod {
  Average,
  Orbit,
  OnEvent,
  OnEventTime,
};

// Optional fixed defaults for an RPC PV. Any field left unset here may be
// supplied at call time by the pvxcall argument structure; if neither the
// config default nor the call argument is present, a sensible zero/empty is
// used. See docs/rpc-forwarding.md.
struct RpcConfig {
  RpcMethod method = RpcMethod::OnEvent;
  // gRPC server "host:port". Empty -> falls back to the IOC-wide default
  // (ServerConfig::rpcDefaultEndpoint).
  std::string endpoint;

  // Source (Average / OnEvent / OnEventTime)
  std::optional<std::string> digitizer;
  std::optional<std::string> subkey;

  // OnEvent
  std::optional<uint32_t> event;
  std::optional<int64_t> deltaNs;

  // OnEventTime / Window offsets
  std::optional<int64_t> eventTimeNs;
  std::optional<int64_t> startNs;
  std::optional<int64_t> endNs;
  std::optional<int64_t> lengthNs;

  // Average window / per-entry mean
  std::optional<bool> perEntryMean;

  // Orbit
  std::optional<std::string> machine;
  std::optional<std::string> section;
  std::optional<int32_t> startIndex;
  std::optional<int32_t> endIndex;
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
  // When set, this PV is an RPC-forwarding PV (no Redis read route required);
  // a `pvxcall <name>` is forwarded to the BpmQuery gRPC server.
  std::optional<RpcConfig> rpc;
};

struct ServerConfig {
  std::string instance;
  std::string nameSpace;
  std::vector<std::string> interfaces;
  std::optional<unsigned short> tcpPort;
  std::optional<unsigned short> udpPort;
  bool autoBeacon = true;
  // IOC-wide default gRPC endpoint ("host:port") used by RPC PVs that do not
  // specify their own `rpc.endpoint`.
  std::string rpcDefaultEndpoint;
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
std::string toString(RpcMethod method);

bool sameReaderTopology(const PVConfig& lhs, const PVConfig& rhs);
bool sameServerConfig(const ServerConfig& lhs, const ServerConfig& rhs);
bool sameRedisConfig(const RedisConfig& lhs, const RedisConfig& rhs);
bool sameRedisBackends(const RedisBackendConfigs& lhs, const RedisBackendConfigs& rhs);
bool sameAlarmStreamConfig(const AlarmStreamConfig& lhs, const AlarmStreamConfig& rhs);

std::string fullPVName(const ServerConfig& server, const PVConfig& pv);
std::string adminPVName(const ServerConfig& server, const std::string& suffix);

}  // namespace redis_pvxs_ioc
