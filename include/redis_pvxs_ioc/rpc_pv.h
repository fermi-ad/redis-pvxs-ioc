#pragma once

#include <map>
#include <atomic>
#include <memory>
#include <string>

#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/grpc_bridge.h"
#include "redis_pvxs_ioc/operation_queue.h"

namespace redis_pvxs_ioc {

struct RpcCallStats {
  std::atomic<uint64_t> dispatched{0}, succeeded{0}, failed{0}, invalid{0}, unauthorized{0};
};

// A SharedPV whose onRPC handler forwards a `pvxcall` to one method of a backend
// gRPC service via a GrpcBridge. Generic: it has no knowledge of the method's
// request/reply schema — that comes from reflection at runtime.
class RpcPV {
public:
  RpcPV(std::shared_ptr<GrpcBridge> bridge, BridgeMethod method,
        std::map<std::string, std::string> defaults,
        std::string name, std::shared_ptr<OperationQueue> queue,
        uint32_t timeoutMs, std::shared_ptr<RpcCallStats> stats);
  ~RpcPV();

  pvxs::server::SharedPV& sharedPV() { return pv_; }

private:
  pvxs::server::SharedPV pv_;
};

// Derive a PV-name leaf from a gRPC method name: CamelCase -> UPPER_SNAKE.
// "Average" -> "AVERAGE", "OnEventTime" -> "ON_EVENT_TIME".
std::string methodToPvLeaf(const std::string& method);

}  // namespace redis_pvxs_ioc
