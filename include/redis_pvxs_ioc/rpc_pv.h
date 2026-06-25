#pragma once

#include <map>
#include <memory>
#include <string>

#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/grpc_bridge.h"

namespace redis_pvxs_ioc {

// A SharedPV whose onRPC handler forwards a `pvxcall` to one method of a backend
// gRPC service via a GrpcBridge. Generic: it has no knowledge of the method's
// request/reply schema — that comes from reflection at runtime.
class RpcPV {
public:
  RpcPV(std::shared_ptr<GrpcBridge> bridge, BridgeMethod method,
        std::map<std::string, std::string> defaults);
  ~RpcPV();

  pvxs::server::SharedPV& sharedPV() { return pv_; }

private:
  std::shared_ptr<GrpcBridge> bridge_;
  BridgeMethod method_;
  std::map<std::string, std::string> defaults_;
  pvxs::server::SharedPV pv_;
};

// Derive a PV-name leaf from a gRPC method name: CamelCase -> UPPER_SNAKE.
// "Average" -> "AVERAGE", "OnEventTime" -> "ON_EVENT_TIME".
std::string methodToPvLeaf(const std::string& method);

}  // namespace redis_pvxs_ioc
