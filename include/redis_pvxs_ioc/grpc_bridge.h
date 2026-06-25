#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <pvxs/data.h>

namespace redis_pvxs_ioc {

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
  explicit GrpcBridge(const std::string& endpoint);
  ~GrpcBridge();
  GrpcBridge(const GrpcBridge&) = delete;
  GrpcBridge& operator=(const GrpcBridge&) = delete;

  // Reflect `service`, cache its descriptors, and return its methods.
  // Throws std::runtime_error on failure (endpoint down, reflection disabled,
  // unknown service).
  std::vector<BridgeMethod> discover(const std::string& service);

  // Invoke a discovered method. `fields` maps a request field (a proto field
  // name, or a dotted path like "source.digitizer", or a unique leaf name) to
  // its string value (numbers parsed with strtoll/strtod, `0x` hex ok). Returns
  // the reply as a pvxs Value mirroring the protobuf reply message. Throws
  // std::runtime_error on a non-OK gRPC status or marshaling error.
  pvxs::Value call(const BridgeMethod& method,
                   const std::map<std::string, std::string>& fields);

  const std::string& endpoint() const { return endpoint_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string endpoint_;
};

}  // namespace redis_pvxs_ioc
