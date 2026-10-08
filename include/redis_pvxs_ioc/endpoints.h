#pragma once

#include <map>
#include <stdexcept>
#include <string>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

enum class EndpointKind { Diagnostic, Value, Alias, RPC };

struct EndpointReservation {
  EndpointKind kind;
  std::string owner;
};

// A generation's complete namespace, including endpoints known only after RPC
// reflection. Build this before changing access policy or live registrations.
class EndpointRegistry {
public:
  void reserve(const std::string& name, EndpointKind kind, const std::string& owner,
               const std::string& path = "endpoints") {
    if (name.empty() || name.find('\0') != std::string::npos)
      throw std::runtime_error(path + ": PV names must be nonempty and must not contain NUL");
    const auto found = entries_.find(name);
    if (found != entries_.end()) {
      const auto detail = found->second.kind == EndpointKind::Diagnostic
          ? "PV name conflicts with reserved metadata PV '" + name + "'"
          : "duplicate served PV name '" + name + "'";
      throw std::runtime_error(path + ": " + detail + " (owned by " + found->second.owner + ")");
    }
    entries_.emplace(name, EndpointReservation{kind, owner});
  }

  void addDiagnostics(const ServerConfig& server) {
    for (const auto& name : adminPVNames(server)) reserve(name, EndpointKind::Diagnostic, "runtime");
  }

  void addPV(const ServerConfig& server, const PVConfig& pv, const std::string& path) {
    const auto names = fullPVNames(server, pv);
    for (size_t index = 0; index < names.size(); ++index) {
      reserve(names[index], index == 0 ? EndpointKind::Value : EndpointKind::Alias, names.front(),
              path + (index == 0 ? ".name" : ".aliases[" + std::to_string(index - 1) + "]"));
    }
  }

  const std::map<std::string, EndpointReservation>& entries() const { return entries_; }

private:
  std::map<std::string, EndpointReservation> entries_;
};

inline EndpointRegistry configuredEndpoints(const AppConfig& config) {
  EndpointRegistry result;
  result.addDiagnostics(config.server);
  for (size_t index = 0; index < config.pvs.size(); ++index)
    result.addPV(config.server, config.pvs[index], "root.pvs[" + std::to_string(index) + "]");
  return result;
}

}  // namespace redis_pvxs_ioc
