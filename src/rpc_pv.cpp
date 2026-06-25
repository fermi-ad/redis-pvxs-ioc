#include "redis_pvxs_ioc/rpc_pv.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <pvxs/data.h>
#include <pvxs/nt.h>

#include "redis_pvxs_ioc/grpc_query_client.h"
#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {
namespace {

// pvxcall sends an NTURI: the user's "name=value" arguments land under a
// `query` sub-structure, e.g. `query.event`, `query.delta_ns`. Some callers
// (or hand-built pvRequests) place fields at the top level instead. Look in
// both places so the handler is tolerant of either shape.
pvxs::Value findArg(const pvxs::Value& request, const std::string& name) {
  if (auto query = request["query"]; query.valid()) {
    if (auto field = query[name]; field.valid() && field.isMarked()) {
      return field;
    }
  }
  if (auto field = request[name]; field.valid() && field.isMarked()) {
    return field;
  }
  return {};
}

std::optional<std::string> argString(const pvxs::Value& request, const std::string& name) {
  auto field = findArg(request, name);
  if (!field.valid()) {
    return std::nullopt;
  }
  try {
    return field.as<std::string>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

template <typename T>
std::optional<T> argInt(const pvxs::Value& request, const std::string& name) {
  auto field = findArg(request, name);
  if (!field.valid()) {
    return std::nullopt;
  }
  // Prefer a native numeric conversion; fall back to parsing a string arg
  // (pvxcall delivers everything as strings).
  T out{};
  if (field.as<T>(out)) {
    return out;
  }
  try {
    const auto text = field.as<std::string>();
    if (text.empty()) {
      return std::nullopt;
    }
    return static_cast<T>(std::strtoll(text.c_str(), nullptr, 0));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<bool> argBool(const pvxs::Value& request, const std::string& name) {
  auto field = findArg(request, name);
  if (!field.valid()) {
    return std::nullopt;
  }
  bool out{};
  if (field.as<bool>(out)) {
    return out;
  }
  if (auto text = argString(request, name)) {
    return *text == "true" || *text == "1" || *text == "yes";
  }
  return std::nullopt;
}

// arg-or-config-default helpers.
std::string pick(const std::optional<std::string>& arg, const std::optional<std::string>& cfg) {
  if (arg) return *arg;
  if (cfg) return *cfg;
  return {};
}

template <typename T>
T pickInt(const std::optional<T>& arg, const std::optional<T>& cfg, T fallback = T{}) {
  if (arg) return *arg;
  if (cfg) return *cfg;
  return fallback;
}

SourceArgs buildSource(const pvxs::Value& request, const RpcConfig& rpc) {
  SourceArgs source;
  source.digitizer = pick(argString(request, "digitizer"), rpc.digitizer);
  source.subkey = pick(argString(request, "subkey"), rpc.subkey);
  return source;
}

// ---- Reply builders --------------------------------------------------------

// QueryReply -> NTScalar (1 value) or NTScalarArray (otherwise). The auxiliary
// fields (times_ns, event_time_ns, units) are attached as extra members so a
// single normative type carries the whole reply.
pvxs::Value buildQueryReply(const QueryResult& result) {
  const bool scalar = result.values.size() == 1;
  const auto valueCode = scalar ? pvxs::TypeCode::Float64 : pvxs::TypeCode::Float64A;

  auto def = pvxs::nt::NTScalar{valueCode, true}.build();
  def += {
      pvxs::members::Int64A("times_ns"),
      pvxs::members::Int64("event_time_ns"),
  };
  auto value = def.create();

  if (scalar) {
    value["value"] = result.values.front();
  } else {
    pvxs::shared_array<double> arr(result.values.begin(), result.values.end());
    value["value"] = arr.freeze();
  }
  if (!result.timesNs.empty()) {
    pvxs::shared_array<int64_t> times(result.timesNs.begin(), result.timesNs.end());
    value["times_ns"] = times.freeze();
  }
  value["event_time_ns"] = result.eventTimeNs;
  if (value["display.units"].valid()) {
    value["display.units"] = result.units;
  }
  applyTimestamp(value);
  return value;
}

// OrbitReply -> NTTable with string name columns + double orbit/intensity
// columns for both planes.
pvxs::Value buildOrbitReply(const OrbitResult& result) {
  auto value = pvxs::nt::NTTable{}
                   .add_column(pvxs::TypeCode::StringA, "h_name", "H BPM")
                   .add_column(pvxs::TypeCode::Float64A, "h_orbit", "H Orbit")
                   .add_column(pvxs::TypeCode::Float64A, "h_intensity", "H Intensity")
                   .add_column(pvxs::TypeCode::StringA, "v_name", "V BPM")
                   .add_column(pvxs::TypeCode::Float64A, "v_orbit", "V Orbit")
                   .add_column(pvxs::TypeCode::Float64A, "v_intensity", "V Intensity")
                   .create();

  const auto strCol = [&](const char* col, const std::vector<std::string>& src) {
    pvxs::shared_array<std::string> arr(src.begin(), src.end());
    value["value." + std::string(col)] = arr.freeze();
  };
  const auto dblCol = [&](const char* col, const std::vector<double>& src) {
    pvxs::shared_array<double> arr(src.begin(), src.end());
    value["value." + std::string(col)] = arr.freeze();
  };

  strCol("h_name", result.hNames);
  dblCol("h_orbit", result.hOrbit);
  dblCol("h_intensity", result.hIntensity);
  strCol("v_name", result.vNames);
  dblCol("v_orbit", result.vOrbit);
  dblCol("v_intensity", result.vIntensity);
  applyTimestamp(value, static_cast<uint64_t>(result.timeNs));
  return value;
}

}  // namespace

std::string resolveRpcEndpoint(const ServerConfig& server, const RpcConfig& rpc) {
  return rpc.endpoint.empty() ? server.rpcDefaultEndpoint : rpc.endpoint;
}

RpcPV::RpcPV(std::string fullName, const RpcConfig& rpc, std::string endpoint)
    : fullName_(std::move(fullName)),
      rpc_(rpc),
      client_(std::make_shared<GrpcQueryClient>(endpoint)),
      pv_(pvxs::server::SharedPV::buildReadonly()) {
  auto client = client_;
  auto config = rpc_;

  pv_.onRPC([client, config](pvxs::server::SharedPV&,
                             std::unique_ptr<pvxs::server::ExecOp>&& op,
                             pvxs::Value&& request) {
    try {
      switch (config.method) {
      case RpcMethod::Average: {
        AverageArgs args;
        args.source = buildSource(request, config);
        args.startNs = pickInt(argInt<int64_t>(request, "start_ns"), config.startNs);
        args.endNs = pickInt(argInt<int64_t>(request, "end_ns"), config.endNs);
        args.lengthNs = pickInt(argInt<int64_t>(request, "length_ns"), config.lengthNs);
        if (auto pem = argBool(request, "per_entry_mean")) {
          args.perEntryMean = *pem;
        } else if (config.perEntryMean) {
          args.perEntryMean = *config.perEntryMean;
        }
        const auto result = client->average(args);
        if (!result.status.ok) {
          op->error("Average failed: " + result.status.message);
          return;
        }
        op->reply(buildQueryReply(result));
        return;
      }
      case RpcMethod::OnEvent: {
        OnEventArgs args;
        args.source = buildSource(request, config);
        args.event = pickInt<uint32_t>(argInt<uint32_t>(request, "event"), config.event);
        args.deltaNs = pickInt(argInt<int64_t>(request, "delta_ns"), config.deltaNs);
        const auto result = client->onEvent(args);
        if (!result.status.ok) {
          op->error("OnEvent failed: " + result.status.message);
          return;
        }
        op->reply(buildQueryReply(result));
        return;
      }
      case RpcMethod::OnEventTime: {
        OnEventTimeArgs args;
        args.source = buildSource(request, config);
        args.eventTimeNs = pickInt(argInt<int64_t>(request, "event_time_ns"), config.eventTimeNs);
        args.startNs = pickInt(argInt<int64_t>(request, "start_ns"), config.startNs);
        args.lengthNs = pickInt(argInt<int64_t>(request, "length_ns"), config.lengthNs);
        const auto result = client->onEventTime(args);
        if (!result.status.ok) {
          op->error("OnEventTime failed: " + result.status.message);
          return;
        }
        op->reply(buildQueryReply(result));
        return;
      }
      case RpcMethod::Orbit: {
        OrbitArgs args;
        args.machine = pick(argString(request, "machine"), config.machine);
        args.section = pick(argString(request, "section"), config.section);
        args.startNs = pickInt(argInt<int64_t>(request, "start_ns"), config.startNs);
        args.endNs = pickInt(argInt<int64_t>(request, "end_ns"), config.endNs);
        args.lengthNs = pickInt(argInt<int64_t>(request, "length_ns"), config.lengthNs);
        args.startIndex = pickInt<int32_t>(argInt<int32_t>(request, "start_index"), config.startIndex);
        args.endIndex = pickInt<int32_t>(argInt<int32_t>(request, "end_index"), config.endIndex);
        const auto result = client->orbit(args);
        if (!result.status.ok) {
          op->error("Orbit failed: " + result.status.message);
          return;
        }
        op->reply(buildOrbitReply(result));
        return;
      }
      case RpcMethod::Slice: {
        SliceArgs args;
        args.source = buildSource(request, config);
        args.endNs = pickInt(argInt<int64_t>(request, "end_ns"), config.endNs);
        args.startIndex = pickInt<int32_t>(argInt<int32_t>(request, "start_index"), config.startIndex);
        args.length = pickInt<int32_t>(argInt<int32_t>(request, "length"), config.length);
        args.stride = pickInt<int32_t>(argInt<int32_t>(request, "stride"), config.stride);
        const auto result = client->slice(args);
        if (!result.status.ok) {
          op->error("Slice failed: " + result.status.message);
          return;
        }
        op->reply(buildQueryReply(result));
        return;
      }
      case RpcMethod::Decimate: {
        DecimateArgs args;
        args.source = buildSource(request, config);
        args.endNs = pickInt(argInt<int64_t>(request, "end_ns"), config.endNs);
        args.factor = pickInt<int32_t>(argInt<int32_t>(request, "factor"), config.factor);
        args.maxPoints = pickInt<int32_t>(argInt<int32_t>(request, "max_points"), config.maxPoints);
        const auto result = client->decimate(args);
        if (!result.status.ok) {
          op->error("Decimate failed: " + result.status.message);
          return;
        }
        op->reply(buildQueryReply(result));
        return;
      }
      }
      op->error("unsupported RPC method");
    } catch (const std::exception& ex) {
      op->error(std::string("RPC handler exception: ") + ex.what());
    }
  });
}

RpcPV::~RpcPV() = default;

}  // namespace redis_pvxs_ioc
