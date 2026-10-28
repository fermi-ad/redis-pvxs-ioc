#include "redis_pvxs_ioc/gateway_pv.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <arpa/inet.h>

namespace redis_pvxs_ioc {
namespace {

bool validServer(const std::string& server) {
  const auto colon = server.find(':');
  if (colon == std::string::npos || !colon || colon + 1 == server.size() ||
      server.find(':', colon + 1) != std::string::npos || server.find('\0') != std::string::npos)
    return false;
  in_addr address{};
  const auto host = server.substr(0, colon);
  if (inet_pton(AF_INET, host.c_str(), &address) != 1) return false;
  const auto bits = ntohl(address.s_addr);
  const auto first = bits >> 24;
  if (first == 0 || first >= 224 || (bits & 0xffu) == 255) return false;
  unsigned port = 0;
  const char* begin = server.data() + colon + 1;
  const char* end = server.data() + server.size();
  const auto parsed = std::from_chars(begin, end, port);
  return parsed.ec == std::errc{} && parsed.ptr == end && port > 0 && port <= 65535;
}

} // namespace

struct GatewayPV::State {
  State(std::string upstream, std::string server)
      : upstream(std::move(upstream)), server(std::move(server)),
        pv(pvxs::server::SharedPV::buildReadonly()) {}

  void event(pvxs::client::Subscription& sub) {
    // Serialize explicit shutdown against each open/post/close sequence.
    std::lock_guard<std::mutex> lock(monitorMutex);
    if (!alive) return;
    for (;;) {
      try {
        auto update = sub.pop();
        if (!update) break;
        if (!alive) break;
        if (!pv.isOpen()) pv.open(update);
        else pv.post(update);
        isConnected = true;
      } catch (const pvxs::client::Finished&) {
        isConnected = false;
        pv.close();
        monitorNeedsRetry = true;
        break;
      } catch (const pvxs::client::Disconnect&) {
        isConnected = false;
        pv.close();
      } catch (const std::exception&) {
        // Remote monitor errors cannot be represented as a valid cached value.
        isConnected = false;
        pv.close();
        monitorNeedsRetry = true;
        break;
      }
    }
  }

  std::string upstream, server;
  pvxs::server::SharedPV pv;
  std::atomic<bool> alive{true}, isConnected{false}, monitorNeedsRetry{false};
  std::mutex monitorMutex;
};

GatewayPV::GatewayPV(std::shared_ptr<State> state, std::shared_ptr<pvxs::client::Context> context)
    : state_(std::move(state)), context_(std::move(context)) {}

std::shared_ptr<GatewayPV> GatewayPV::create(pvxs::client::Context context,
                                             std::string upstreamName,
                                             std::string upstreamServer) {
  if (upstreamName.empty() || upstreamName.find('\0') != std::string::npos || !validServer(upstreamServer))
    throw std::invalid_argument("gateway requires a name and a unicast IPv4 TCP server");
  auto contextOwner = std::make_shared<pvxs::client::Context>(std::move(context));
  auto state = std::make_shared<State>(std::move(upstreamName), std::move(upstreamServer));
  auto result = std::shared_ptr<GatewayPV>(new GatewayPV(state, contextOwner));
  result->startMonitor();
  return result;
}

void GatewayPV::startMonitor() {
  monitor_ = context_->monitor(state_->upstream)
      .server(state_->server)
      .maskDisconnected(false).syncCancel(false)
      .event([weak = std::weak_ptr<State>(state_)](pvxs::client::Subscription& sub) {
        if (auto state = weak.lock()) state->event(sub);
      }).exec();
}

GatewayPV::~GatewayPV() {
  // A binding's last owner can be released by a server callback. Do not
  // explicitly cancel there: implicit cancellation was configured async.
  state_->alive = false;
  monitor_.reset();
}

pvxs::server::SharedPV& GatewayPV::sharedPV() { return state_->pv; }
void GatewayPV::deactivate() {
  state_->alive = false;
  state_->isConnected = false;
  if (monitor_) monitor_->cancel();
  std::lock_guard<std::mutex> lock(state_->monitorMutex);
  state_->pv.close();
}
void GatewayPV::pump() {
  if (!state_->alive || !state_->monitorNeedsRetry || std::chrono::steady_clock::now() < nextMonitorRetry_) return;
  nextMonitorRetry_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  // Explicit cancel synchronizes with the previous callback. Clear its retry
  // flag only after it is gone so an immediate failure of the new monitor is
  // retained for the next bounded attempt.
  if (monitor_) monitor_->cancel();
  state_->monitorNeedsRetry = false;
  try {
    startMonitor();
  } catch (const std::exception&) {
    state_->monitorNeedsRetry = true;
  }
}
bool GatewayPV::connected() const { return state_->alive && state_->isConnected; }

} // namespace redis_pvxs_ioc
