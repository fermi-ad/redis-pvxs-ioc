#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <pvxs/client.h>
#include <pvxs/sharedpv.h>

namespace redis_pvxs_ioc {

// One explicitly selected, read-only upstream PVA channel. The caller supplies
// a numeric unicast IPv4 TCP endpoint; monitors connect without search. GETs
// serve the current monitor snapshot. An upstream disconnect closes the local
// SharedPV until a new snapshot, possibly of a different type, arrives.
class GatewayPV {
public:
  static std::shared_ptr<GatewayPV> create(pvxs::client::Context context,
                                           std::string upstreamName,
                                           std::string upstreamServer);
  ~GatewayPV();
  GatewayPV(const GatewayPV&) = delete;
  GatewayPV& operator=(const GatewayPV&) = delete;

  pvxs::server::SharedPV& sharedPV();
  // Deactivation is final and must be called outside a PVXS server callback;
  // it closes local channels and cancels the monitor.
  void deactivate();
  // Retry terminal upstream monitor errors. Transport disconnects are retried
  // by PVXS itself.
  void pump();
  bool connected() const;

private:
  struct State;
  GatewayPV(std::shared_ptr<State> state, std::shared_ptr<pvxs::client::Context> context);
  void startMonitor();
  std::shared_ptr<State> state_;
  std::shared_ptr<pvxs::client::Context> context_;
  std::shared_ptr<pvxs::client::Subscription> monitor_;
  std::chrono::steady_clock::time_point nextMonitorRetry_{};
};

} // namespace redis_pvxs_ioc
