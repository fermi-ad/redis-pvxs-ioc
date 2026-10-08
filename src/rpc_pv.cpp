#include "redis_pvxs_ioc/rpc_pv.h"
#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/queued_exec.h"

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
std::map<std::string, std::string> extractArgs(const pvxs::Value& request, size_t remaining) {
  std::map<std::string, std::string> out;
  const bool uri = request.id().find("epics:nt/NTURI:") == 0;
  const auto body = uri ? request["query"] : request;
  if (!body || body.type() != pvxs::TypeCode::Struct)
    throw std::runtime_error("RPC request/query must be a structure");
  size_t count = 0;
  for (auto fld : body.iall()) {
    if (++count > 4096) throw std::runtime_error("RPC argument structure exceeds field limit");
    if (fld.type() == pvxs::TypeCode::Struct) continue;
    const auto name = body.nameOf(fld);
    if (fld.type().isarray() || fld.type().kind() == pvxs::Kind::Compound)
      throw std::runtime_error("RPC argument must be scalar: " + name);
    auto value = fld.as<std::string>();
    if (name.size() > remaining || value.size() > remaining - name.size())
      throw std::runtime_error("RPC arguments exceed payload limit");
    remaining -= name.size() + value.size();
    out.emplace(name, std::move(value));
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
             std::map<std::string, std::string> defaults,
             std::string name, std::shared_ptr<OperationQueue> queue,
             uint32_t timeoutMs, std::shared_ptr<RpcCallStats> stats)
    : pv_(pvxs::server::SharedPV::buildReadonly()) {
  auto defaultsRef = std::make_shared<const GrpcBridge::Fields>(std::move(defaults));
  pv_.onRPC([bridge, method, defaultsRef, name, queue, timeoutMs, stats](pvxs::server::SharedPV&,
                                     std::unique_ptr<pvxs::server::ExecOp>&& op,
                                     pvxs::Value&& request) {
    const auto deadline = OperationQueue::Clock::now() + std::chrono::milliseconds(timeoutMs);
    auto pending = QueuedExec::create(std::move(op), queue);
    try {
      auto fields = extractArgs(request, bridge->maxMessageBytes());
      // Reserve argument storage and bookkeeping, including empty requests.
      // Defaults/descriptors are shared; active worker protobuf/reply buffers
      // are separately limited by max_payload_bytes.
      size_t bytes = 1024;
      for (const auto& field : fields) bytes += 128 + field.first.size() + field.second.size();
      auto control = std::make_shared<GrpcCallControl>();
      pending->wake([control] { control->cancel(); });
      const auto ticket = queue->submit({name, bytes, deadline,
          [pending, bridge, method, defaultsRef, fields = std::move(fields), deadline, control, stats] {
            if (pending->stopped()) return;
            bool sent = false, denied = false;
            try {
              auto result = bridge->call(method, fields, *defaultsRef, deadline, control, [&] {
                const auto operation = pending->operation();
                if (!operation || pending->stopped()) return false;
                // RPC arguments may contain secrets; the dispatch audit needs
                // the channel identity/rights, not a second copy of its payload.
                if (!authorizeWriteDispatch(*operation, {})) {
                  denied = true; ++stats->unauthorized; return false;
                }
                if (pending->stopped()) return false;
                sent = true; ++stats->dispatched; return true;
              });
              ++stats->succeeded;
              pending->reply(result);
            } catch (const std::exception& error) {
              if (sent) ++stats->failed;
              else if (!denied && !pending->stopped()) ++stats->invalid;
              pending->error(method.method + ": " + error.what());
            }
          }, [pending](OperationFailure, const std::string& message) { pending->error(message); }});
      pending->ticket(queue, ticket);
    } catch (const std::exception& e) {
      ++stats->invalid;
      pending->error(method.method + ": " + e.what());
    }
  });
}

RpcPV::~RpcPV() = default;

}  // namespace redis_pvxs_ioc
