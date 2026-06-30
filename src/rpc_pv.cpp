#include "redis_pvxs_ioc/rpc_pv.h"

#include <cctype>
#include <exception>
#include <utility>

#include <pvxs/data.h>
#include <pvxs/srvcommon.h>

namespace redis_pvxs_ioc {
namespace {

// Collect pvxcall arguments into a {field-name -> string} map. pvxcall sends an
// NTURI whose user args are the marked leaf fields under `query`; tolerate a
// flat (non-NTURI) request too. Names are taken relative to the request, with a
// leading "query." stripped, so they line up with proto field names.
std::map<std::string, std::string> extractArgs(const pvxs::Value& request) {
  std::map<std::string, std::string> out;
  for (auto fld : request.imarked()) {
    std::string name = request.nameOf(fld);
    if (name == "scheme" || name == "path" || name == "query") continue;
    if (name.rfind("query.", 0) == 0) name = name.substr(6);
    try {
      out[name] = fld.as<std::string>();  // throws for non-scalar (struct) fields
    } catch (const std::exception&) {
    }
  }
  return out;
}

}  // namespace

std::string methodToPvLeaf(const std::string& method) {
  std::string out;
  for (std::size_t i = 0; i < method.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(method[i]);
    if (std::isupper(c) && i > 0 &&
        !std::isupper(static_cast<unsigned char>(method[i - 1]))) {
      out.push_back('_');
    }
    out.push_back(static_cast<char>(std::toupper(c)));
  }
  return out;
}

RpcPV::RpcPV(std::shared_ptr<GrpcBridge> bridge, BridgeMethod method,
             std::map<std::string, std::string> defaults)
    : bridge_(std::move(bridge)),
      method_(std::move(method)),
      defaults_(std::move(defaults)),
      pv_(pvxs::server::SharedPV::buildReadonly()) {
  auto bridgeRef = bridge_;
  auto methodRef = method_;
  auto defaultsRef = defaults_;
  pv_.onRPC([bridgeRef, methodRef, defaultsRef](pvxs::server::SharedPV&,
                                                std::unique_ptr<pvxs::server::ExecOp>&& op,
                                                pvxs::Value&& request) {
    try {
      std::map<std::string, std::string> fields = defaultsRef;    // config defaults
      for (auto& [k, v] : extractArgs(request)) fields[k] = v;     // call overrides
      op->reply(bridgeRef->call(methodRef, fields));
    } catch (const std::exception& e) {
      op->error(methodRef.method + ": " + e.what());
    }
  });
}

RpcPV::~RpcPV() = default;

}  // namespace redis_pvxs_ioc
