#pragma once

#include <map>
#include <chrono>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

#include <pvxs/data.h>

namespace redis_pvxs_ioc {

class RpcDiscoveryUnavailable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Cancellation remains effective before dispatch, while queued, and during
// network I/O. Each call/reflection attempt owns a separate context.
class GrpcCallControl {
public:
  GrpcCallControl();
  ~GrpcCallControl();
  void cancel();
private:
  friend class GrpcBridge;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// One RPC method discovered on a backend gRPC service via reflection.
struct BridgeMethod {
  std::string service;  // fully-qualified, e.g. "bpm.query.v1.BpmQuery"
  std::string method;   // e.g. "Average"
};

// Generic PVA <-> gRPC bridge for ONE endpoint.
//
// Uses gRPC server reflection to learn a service's methods and message schema
// at runtime, marshals a string field map into the method's protobuf request,
// performs a generic unary call, and marshals the protobuf reply back into a
// pvxs Value. It compiles in NO application proto — only the standard gRPC
// reflection proto — so the IOC image is not specific to any one backend.
class GrpcBridge {
public:
  using Fields = std::map<std::string, std::string>;
  using MethodDefaults = std::map<std::string, Fields>;
  explicit GrpcBridge(const std::string& endpoint, uint32_t discoveryTimeoutMs = 3000,
                      uint32_t timeoutMs = 10000, size_t maxMessageBytes = 32u * 1024u * 1024u);
  ~GrpcBridge();
  GrpcBridge(const GrpcBridge&) = delete;
  GrpcBridge& operator=(const GrpcBridge&) = delete;

  // Reflect `service`, cache its descriptors, and return its methods.
  // Throws std::runtime_error on failure (endpoint down, reflection disabled,
  // unknown service).
  std::vector<BridgeMethod> discover(const std::string& service,
                                   std::shared_ptr<GrpcCallControl> control = {});

  // Validate defaults against the reflected schemas and return canonical paths
  // for each method. Shared fields must occur in at least one method.
  MethodDefaults prepareDefaults(const std::string& service, const Fields& shared,
                                  const MethodDefaults& methods);

  // Invoke a discovered method. `fields` maps a request field (a proto field
  // name, or a dotted path like "source.digitizer", or a unique leaf name) to
  // its string value (numbers parsed with strtoll/strtod, `0x` hex ok). Returns
  // the reply as a pvxs Value mirroring the protobuf reply message. Throws
  // std::runtime_error on a non-OK gRPC status or marshaling error.
  pvxs::Value call(const BridgeMethod& method,
                   const Fields& fields, const Fields& defaults,
                   std::chrono::steady_clock::time_point deadline,
                   const std::shared_ptr<GrpcCallControl>& control,
                   const std::function<bool()>& beforeDispatch);

  const std::string& endpoint() const { return endpoint_; }
  size_t maxMessageBytes() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string endpoint_;
};

}  // namespace redis_pvxs_ioc
