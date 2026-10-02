#pragma once

#include "redis_pvxs_ioc/config.h"
#include <functional>
#include <map>
#include <memory>
#include <pvxs/sharedpv.h>
#include <pvxs/source.h>

namespace redis_pvxs_ioc {

struct PVBinding {
  pvxs::server::SharedPV pv;
  std::shared_ptr<void> owner;
  AccessAssignment access;
};
using PVBindings = std::map<std::string, PVBinding>;

// Prepare an entire namespace off-line, then replace one published snapshot.
// Registrations own channels by served name; retiring an alias never closes the
// SharedPV also used by its canonical name or other aliases.
class PVRegistry final : public pvxs::server::Source {
public:
  using Channel = std::unique_ptr<pvxs::server::ChannelControl>;
  using Wrapper = std::function<Channel(Channel)>;
  using Wrappers = std::map<std::string, Wrapper>;
  struct Prepared;
  PVRegistry();
  ~PVRegistry();
  PVBindings bindings() const;
  std::shared_ptr<Prepared> prepare(PVBindings desired, const Wrappers& wrappers = {}) const;
  // beforeCommit is the last fallible action. On false it must leave external
  // state unchanged. After it succeeds, snapshot publication cannot allocate.
  // Publications are serialized; readers serve the previous immutable snapshot
  // throughout beforeCommit, even when it waits for external work.
  bool publish(const std::shared_ptr<Prepared>& prepared,
               const std::function<bool(std::string&)>& beforeCommit, std::string& error);
  void finish(const std::shared_ptr<Prepared>& prepared);
  void clear();
  void onSearch(Search& search) override;
  void onCreate(Channel&& channel) override;
  List onList() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redis_pvxs_ioc
