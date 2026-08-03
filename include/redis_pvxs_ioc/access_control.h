#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <pvxs/server.h>
#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct AccessStatus {
  bool enabled = false;
  uint64_t generation = 0;
  uint64_t activeClients = 0;
  uint64_t deniedReads = 0;
  uint64_t deniedWrites = 0;
  uint64_t rightsChanges = 0;
  std::string lastStatus = "disabled";
  std::string lastError;
  std::string policyFingerprint;
  std::string watchStatus = "disabled";
};

class AccessController : public std::enable_shared_from_this<AccessController> {
public:
  explicit AccessController(AccessConfig config);
  ~AccessController();

  AccessController(const AccessController&) = delete;
  AccessController& operator=(const AccessController&) = delete;

  bool start(const std::set<std::string>& requiredAsgs, std::string& error);
  bool reconfigure(const AccessConfig& config,
                   const std::set<std::string>& requiredAsgs,
                   std::string& error);
  bool restorePrevious(std::string& error);
  bool reload(const std::string& trigger, std::string& error);
  void requestReload(const std::string& trigger);
  void pump();

  void addPV(const std::string& name,
             const pvxs::server::SharedPV& pv,
             const AccessAssignment& assignment);
  void removePV(const std::string& name);
  void setAssignment(const std::string& name, const AccessAssignment& assignment);
  std::shared_ptr<pvxs::server::Source> source() const;

  AccessStatus status() const;

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

// Parse/expand/inspect an ACF without starting a PVA server.  Used by
// --check-config and focused tests.  The returned fingerprint identifies the
// expanded policy bytes; it is not a cryptographic signature.
bool validateAccessPolicy(const AccessConfig& config,
                          const std::set<std::string>& requiredAsgs,
                          std::string& fingerprint,
                          std::string& error);

}  // namespace redis_pvxs_ioc
