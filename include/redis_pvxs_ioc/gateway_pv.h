#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <pvxs/client.h>
#include <pvxs/sharedpv.h>

#include "redis_pvxs_ioc/operation_queue.h"

namespace redis_pvxs_ioc {

// One explicitly selected upstream PVA channel, exported under a local name.
// The caller supplies a context and a numeric unicast IPv4 TCP endpoint; all
// monitor and put operations connect directly to that endpoint without search.
// GETs serve the current monitor snapshot; an upstream disconnect closes the
// local SharedPV until a new snapshot, possibly of a different type, arrives.
class GatewayPV {
public:
  static std::shared_ptr<GatewayPV> create(pvxs::client::Context context,
                                           std::string upstreamName,
                                           std::string upstreamServer,
                                           std::string servedName,
                                           std::shared_ptr<OperationQueue> queue,
                                           uint32_t timeoutMs,
                                           size_t maxPayloadBytes);
  ~GatewayPV();
  GatewayPV(const GatewayPV&) = delete;
  GatewayPV& operator=(const GatewayPV&) = delete;

  pvxs::server::SharedPV& sharedPV();
  // Creation stages the monitor with writes disabled. Activate only after the
  // namespace is committed. retireWrites() is the nonthrowing, final cutover
  // fence for use inside a registry publication: requests scheduled before the
  // fence are dispatched; requests behind it cannot be scheduled. Deactivation
  // is final and must be called outside a PVXS server callback; it closes local
  // channels and cancels the monitor.
  void activate();
  void retireWrites() noexcept;
  void deactivate();
  // Call from the application loop to retry a terminal upstream monitor error.
  // Transport disconnects are retried by PVXS itself.
  void pump();
  bool connected() const;

private:
  struct State;
  GatewayPV(std::shared_ptr<State> state, std::shared_ptr<pvxs::client::Context> context);
  void startMonitor();
  std::shared_ptr<State> state_;
  std::shared_ptr<pvxs::client::Context> context_;
  std::shared_ptr<pvxs::client::Subscription> monitor_;
  OperationQueue::Clock::time_point nextMonitorRetry_{};
};

} // namespace redis_pvxs_ioc
