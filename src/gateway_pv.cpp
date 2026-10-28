#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include "redis_pvxs_ioc/gateway_pv.h"

#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/queued_exec.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <arpa/inet.h>

#include <pvxs/data.h>
#include <pvxs/sharedArray.h>

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

// Bound arbitrary values, including arrays of structs/unions and variant
// fields, without reducing them to a particular normative type. Account for
// per-field/element storage as well as the variable-length payload.
class ValueCharge {
public:
  explicit ValueCharge(size_t limit) : limit_(limit) {}
  size_t measure(const pvxs::Value& value) { visit(value, 0); return used_; }
private:
  void add(size_t n) {
    if (n > limit_ - used_) throw std::runtime_error("write payload exceeds max_payload_bytes");
    used_ += n;
  }
  void multiply(size_t count, size_t width) {
    if (width && count > (limit_ - used_) / width)
      throw std::runtime_error("write payload exceeds max_payload_bytes");
    add(count * width);
  }
  void visit(const pvxs::Value& value, unsigned depth) {
    if (!value) return;
    if (depth > 64) throw std::runtime_error("write structure exceeds nesting limit");
    add(32); // field storage and queue bookkeeping
    const auto type = value.type();
    if (type == pvxs::TypeCode::Struct) {
      for (const auto& child : value.ichildren()) visit(child, depth + 1);
    } else if (type.isarray()) {
      const auto array = value.as<pvxs::shared_array<const void>>();
      if (array.empty()) return;
      if (array.original_type() == pvxs::ArrayType::String) {
        const auto strings = array.castTo<const std::string>();
        multiply(strings.size(), sizeof(std::string));
        for (const auto& entry : strings) add(entry.size());
      } else if (array.original_type() == pvxs::ArrayType::Value) {
        const auto values = array.castTo<const pvxs::Value>();
        multiply(values.size(), sizeof(pvxs::Value));
        for (const auto& entry : values) visit(entry, depth + 1);
      } else {
        multiply(array.size(), pvxs::elementSize(array.original_type()));
      }
    } else if (type.kind() == pvxs::Kind::Compound) {
      visit(value.as<pvxs::Value>(), depth + 1);
    } else if (type == pvxs::TypeCode::String) {
      add(value.as<std::string>().size());
    } else {
      add(type.size());
    }
  }
  size_t limit_, used_ = 0;
};

struct PutWaiter {
  std::mutex mutex;
  std::condition_variable changed;
  pvxs::Value prototype;
  bool initialized = false, finished = false;
  std::string error;

  void complete(pvxs::client::Result&& result) {
    std::string message;
    try { result(); } catch (const std::exception& e) { message = e.what(); }
    std::lock_guard<std::mutex> lock(mutex);
    if (!finished) {
      error = std::move(message);
      finished = true;
    }
    changed.notify_all();
  }
};

} // namespace

struct GatewayPV::State : public std::enable_shared_from_this<GatewayPV::State> {
  State(std::weak_ptr<pvxs::client::Context> context, std::string upstream, std::string server, std::string served,
        std::shared_ptr<OperationQueue> queue, uint32_t timeout, size_t limit)
      : context(std::move(context)), upstream(std::move(upstream)), server(std::move(server)), served(std::move(served)),
        queue(std::move(queue)), timeoutMs(timeout), payloadLimit(limit),
        pv(pvxs::server::SharedPV::buildMailbox()) {}

  void event(pvxs::client::Subscription& sub) {
    // Only one monitor owns this state. The mutex also serializes an explicit
    // shutdown against an in-progress open/post/close sequence.
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
        ++epoch;
        pv.close();
        monitorNeedsRetry = true;
        break;
      } catch (const pvxs::client::Disconnect&) {
        isConnected = false;
        ++epoch;
        pv.close();
      } catch (const std::exception&) {
        // Remote monitor errors cannot be represented as a valid cached value.
        isConnected = false;
        ++epoch;
        pv.close();
        monitorNeedsRetry = true;
        break;
      }
    }
  }

  void put(std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
    if (!alive || !active || !isConnected) { op->error("upstream unavailable"); return; }
    const auto admittedEpoch = epoch.load();
    size_t bytes;
    pvxs::Value pvRequest;
    try {
      bytes = ValueCharge(payloadLimit).measure(value);
      if (op->pvRequest()) pvRequest = op->pvRequest().clone();
    } catch (const std::exception& e) {
      op->error(e.what()); return;
    }
    const auto deadline = OperationQueue::Clock::now() + std::chrono::milliseconds(timeoutMs);
    auto pending = QueuedExec::create(std::move(op), queue);
    auto self = shared_from_this();
    const auto ticket = queue->submit({served, bytes, deadline,
        [self, pending, admittedEpoch, deadline, pvRequest = std::move(pvRequest), value = std::move(value)]() mutable {
          self->dispatch(pending, admittedEpoch, deadline, std::move(pvRequest), std::move(value));
        },
        [pending](OperationFailure, const std::string& message) { pending->error(message); }});
    pending->ticket(queue, ticket);
  }

  void dispatch(const std::shared_ptr<QueuedExec>& pending, uint64_t admittedEpoch,
                OperationQueue::Clock::time_point deadline, pvxs::Value pvRequest, pvxs::Value value) {
    if (pending->stopped()) return;
    if (!alive || !active || !isConnected || epoch != admittedEpoch) {
      pending->error("upstream connection changed before dispatch; not retried"); return;
    }
    try {
      auto context = this->context.lock();
      if (!context) { pending->error("gateway unavailable"); return; }
      auto waiter = std::make_shared<PutWaiter>();
      auto builder = context->put(upstream);
      builder.server(server);
      if (pvRequest) builder.rawRequest(pvRequest);
      // PVXS autoExec PUTs may replay after an uncertain disconnect. Manual
      // execution makes an in-flight disconnect fail instead of resending it.
      auto operation = builder.autoExec(false)
          .onInit([waiter](const pvxs::Value& prototype) {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            waiter->prototype = prototype.cloneEmpty();
            waiter->initialized = true;
            waiter->changed.notify_all();
          }).result([waiter](pvxs::client::Result&& result) { waiter->complete(std::move(result)); }).exec();
      pending->wake([waiter] {
        std::lock_guard<std::mutex> lock(waiter->mutex);
        waiter->changed.notify_all();
      });
      pvxs::Value prototype;
      std::string setupError;
      {
        std::unique_lock<std::mutex> lock(waiter->mutex);
        waiter->changed.wait_until(lock, deadline, [&] {
          return waiter->initialized || waiter->finished || pending->stopped();
        });
        if (pending->stopped()) return;
        if (waiter->finished) setupError = "upstream put setup failed; not retried: " + waiter->error;
        else if (!waiter->initialized) setupError = "total operation deadline exceeded; not retried";
        else prototype = waiter->prototype;
      }
      if (!setupError.empty()) { pending->error(setupError); return; }
      if (!alive || !active || !isConnected || epoch != admittedEpoch) {
        pending->error("upstream connection changed before dispatch; not retried"); return;
      }
      if (!prototype.equalType(value)) { pending->error("upstream put type changed"); return; }
      auto outgoing = prototype.cloneEmpty();
      outgoing.assign(value);
      std::string dispatchError;
      {
        // retireWrites() takes the same fence in the last publication action.
        // Scheduling before that fence counts as dispatch; queued work behind
        // it can never be sent by this retired mapping.
        std::lock_guard<std::mutex> lock(dispatchMutex);
        auto request = pending->operation();
        if (!request || pending->stopped()) return;
        if (!alive || !active || !isConnected || epoch != admittedEpoch) {
          dispatchError = "upstream connection changed before dispatch; not retried";
        } else if (!authorizeWriteDispatch(*request, value)) {
          dispatchError = "access denied";
        } else if (pending->stopped()) {
          return;
        } else if (!alive || !active || !isConnected || epoch != admittedEpoch) {
          dispatchError = "upstream connection changed before dispatch; not retried";
        } else {
          operation->reExecPut(outgoing, [waiter](pvxs::client::Result&& result) { waiter->complete(std::move(result)); });
        }
      }
      if (!dispatchError.empty()) { pending->error(dispatchError); return; }
      std::string completionError;
      {
        std::unique_lock<std::mutex> lock(waiter->mutex);
        waiter->changed.wait_until(lock, deadline, [&] { return waiter->finished || pending->stopped(); });
        if (pending->stopped()) return;
        if (!waiter->finished) completionError = "total operation deadline exceeded; not retried";
        else if (!waiter->error.empty()) completionError = "upstream put failed; not retried: " + waiter->error;
      }
      if (!completionError.empty()) { pending->error(completionError); return; }
      pending->reply();
    } catch (const std::exception& e) {
      if (!pending->stopped()) pending->error(std::string("upstream put failed; not retried: ") + e.what());
    }
  }

  std::weak_ptr<pvxs::client::Context> context;
  std::string upstream, server, served;
  std::shared_ptr<OperationQueue> queue;
  uint32_t timeoutMs;
  size_t payloadLimit;
  pvxs::server::SharedPV pv;
  std::atomic<bool> alive{true}, active{false}, retired{false}, isConnected{false}, monitorNeedsRetry{false};
  std::atomic<uint64_t> epoch{0};
  std::mutex monitorMutex, dispatchMutex;
};

GatewayPV::GatewayPV(std::shared_ptr<State> state, std::shared_ptr<pvxs::client::Context> context)
    : state_(std::move(state)), context_(std::move(context)) {}

std::shared_ptr<GatewayPV> GatewayPV::create(pvxs::client::Context context,
                                             std::string upstreamName,
                                             std::string upstreamServer,
                                             std::string servedName,
                                             std::shared_ptr<OperationQueue> queue,
                                             uint32_t timeoutMs,
                                             size_t maxPayloadBytes) {
  if (upstreamName.empty() || upstreamName.find('\0') != std::string::npos ||
      servedName.empty() || servedName.find('\0') != std::string::npos ||
      !validServer(upstreamServer) || !queue || !timeoutMs || !maxPayloadBytes)
    throw std::invalid_argument("gateway requires names, a unicast IPv4 TCP server, queue, timeout and payload limit");
  auto contextOwner = std::make_shared<pvxs::client::Context>(std::move(context));
  auto state = std::make_shared<State>(contextOwner, std::move(upstreamName), std::move(upstreamServer),
                                       std::move(servedName), std::move(queue), timeoutMs, maxPayloadBytes);
  auto result = std::shared_ptr<GatewayPV>(new GatewayPV(state, contextOwner));
  state->pv.onPut([weak = std::weak_ptr<State>(state)](pvxs::server::SharedPV&,
                          std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
    if (auto state = weak.lock()) state->put(std::move(op), std::move(value));
    else op->error("gateway unavailable");
  });
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
  // Normal retirement calls deactivate() on the application thread first.
  retireWrites();
  state_->alive = false;
  monitor_.reset();
}

pvxs::server::SharedPV& GatewayPV::sharedPV() { return state_->pv; }
void GatewayPV::activate() {
  std::lock_guard<std::mutex> lock(state_->dispatchMutex);
  if (!state_->alive || state_->retired) throw std::logic_error("cannot activate a retired gateway");
  state_->active = true;
}
void GatewayPV::retireWrites() noexcept {
  std::lock_guard<std::mutex> lock(state_->dispatchMutex);
  state_->active = false;
  state_->retired = true;
  ++state_->epoch;
}
void GatewayPV::deactivate() {
  retireWrites();
  state_->alive = false;
  state_->isConnected = false;
  if (monitor_) monitor_->cancel();
  std::lock_guard<std::mutex> lock(state_->monitorMutex);
  state_->pv.close();
}
void GatewayPV::pump() {
  if (!state_->alive || !state_->monitorNeedsRetry || OperationQueue::Clock::now() < nextMonitorRetry_) return;
  nextMonitorRetry_ = OperationQueue::Clock::now() + std::chrono::seconds(5);
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
