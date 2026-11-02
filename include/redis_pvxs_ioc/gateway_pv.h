#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <pvxs/client.h>
#include <pvxs/source.h>

namespace redis_pvxs_ioc {

// A canonical upstream channel. Aliases attach through this object while each
// downstream operation retains its own upstream request and completion.
class GatewayPV {
public:
  using Channel = std::unique_ptr<pvxs::server::ChannelControl>;
  struct Limits {
    size_t maxChannels = 4096;
    size_t maxSetups = 4096;
    size_t maxInFlight = 256;
    size_t maxMonitors = 256;
    size_t maxPayloadBytes = 32u * 1024u * 1024u;
    size_t maxInFlightBytes = 64u * 1024u * 1024u;
    size_t maxMonitorQueue = 64;
    uint32_t timeoutMs = 5000;
  };
  struct Status {
    bool connected = false;
    uint64_t channels = 0, operations = 0, monitors = 0, inFlight = 0;
    uint64_t completed = 0, failed = 0, rejected = 0, uncertain = 0;
  };
  // One application shares this budget across every canonical upstream.
  class Budget {
  public:
    explicit Budget(const Limits& limits);
    struct Impl;
    std::shared_ptr<Impl> impl;
  };
  static std::shared_ptr<GatewayPV> create(pvxs::client::Context context,
                                          std::string upstreamName,
                                          std::string upstreamServer);
  static std::shared_ptr<GatewayPV> create(pvxs::client::Context context,
                                          std::string upstreamName,
                                          std::string upstreamServer,
                                          const Limits& limits);
  static std::shared_ptr<GatewayPV> create(pvxs::client::Context context,
                                          std::string upstreamName,
                                          std::string upstreamServer,
                                          const Limits& limits,
                                          std::shared_ptr<Budget> budget);
  ~GatewayPV();
  GatewayPV(const GatewayPV&) = delete;
  GatewayPV& operator=(const GatewayPV&) = delete;

  void attach(Channel&& channel);
  void deactivate();
  void pump();
  bool connected() const;
  Status status() const;

private:
  struct State;
  explicit GatewayPV(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

} // namespace redis_pvxs_ioc
