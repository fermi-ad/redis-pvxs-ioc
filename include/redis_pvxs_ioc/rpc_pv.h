#pragma once

#include <memory>
#include <string>

#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

class GrpcQueryClient;

// Owns a SharedPV with an installed onRPC() handler that forwards a `pvxcall`
// to the BpmQuery gRPC server and maps the reply into a PVA structure. One
// instance per configured RPC PV.
class RpcPV {
public:
  RpcPV(std::string fullName, const RpcConfig& rpc, std::string endpoint);
  ~RpcPV();

  const std::string& fullName() const { return fullName_; }
  pvxs::server::SharedPV& sharedPV() { return pv_; }

private:
  std::string fullName_;
  RpcConfig rpc_;
  std::shared_ptr<GrpcQueryClient> client_;
  pvxs::server::SharedPV pv_;
};

// Resolve the effective gRPC endpoint for an RPC PV (rpc.endpoint, else the
// server-wide default).
std::string resolveRpcEndpoint(const ServerConfig& server, const RpcConfig& rpc);

}  // namespace redis_pvxs_ioc
