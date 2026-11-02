#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include "redis_pvxs_ioc/gateway_pv.h"
#include "redis_pvxs_ioc/access_control.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>
#include <arpa/inet.h>

#include <pvxs/proxy.h>
#include <pvxs/sharedArray.h>

namespace redis_pvxs_ioc {
namespace {
using Clock = std::chrono::steady_clock;
using Exec = std::unique_ptr<pvxs::server::ExecOp>;

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

// Charge the complete dynamic value, including variant and compound arrays.
// The depth and byte ceilings also bound work before an operation is admitted.
class ValueCharge {
  size_t limit_, used_ = 0;
  void add(size_t n) {
    if (n > limit_ - used_) throw std::runtime_error("gateway payload exceeds byte limit");
    used_ += n;
  }
  void multiply(size_t n, size_t width) {
    if (width && n > (limit_ - used_) / width) throw std::runtime_error("gateway payload exceeds byte limit");
    add(n * width);
  }
  void visit(const pvxs::Value& value, unsigned depth) {
    if (!value) return;
    if (depth > 64) throw std::runtime_error("gateway value exceeds nesting limit");
    add(32);
    const auto type = value.type();
    if (type == pvxs::TypeCode::Struct) {
      for (const auto& child : value.ichildren()) visit(child, depth + 1);
    } else if (type.isarray()) {
      const auto array = value.as<pvxs::shared_array<const void>>();
      if (array.empty()) return;
      if (array.original_type() == pvxs::ArrayType::String) {
        const auto values = array.castTo<const std::string>();
        multiply(values.size(), sizeof(std::string));
        for (const auto& entry : values) add(entry.size());
      } else if (array.original_type() == pvxs::ArrayType::Value) {
        const auto values = array.castTo<const pvxs::Value>();
        multiply(values.size(), sizeof(pvxs::Value));
        for (const auto& entry : values) visit(entry, depth + 1);
      } else multiply(array.size(), pvxs::elementSize(array.original_type()));
    } else if (type.kind() == pvxs::Kind::Compound) visit(value.as<pvxs::Value>(), depth + 1);
    else if (type == pvxs::TypeCode::String) add(value.as<std::string>().size());
    else add(type.size());
  }
public:
  explicit ValueCharge(size_t limit) : limit_(limit) {}
  size_t measure(const pvxs::Value& value) { visit(value, 0); return used_; }
};

bool processingGet(const pvxs::Value& request) {
  const auto field = request["record._options.process"];
  if (!field) return false;
  std::string text;
  if (field.as(text)) return text != "false" && text != "0";
  bool value = false;
  return !field.as(value) || value;
}
} // namespace

struct GatewayPV::Budget::Impl : std::enable_shared_from_this<GatewayPV::Budget::Impl> {
  enum Kind { Channel, Setup, Monitor, Operation, MonitorValue };
  const Limits limits;
  std::mutex mutex;
  size_t channels = 0, setups = 0, monitors = 0, operations = 0, bytes = 0;
  struct Lease {
    std::shared_ptr<Impl> owner;
    Kind kind;
    size_t bytes;
    Lease(std::shared_ptr<Impl> owner, Kind kind, size_t bytes) : owner(std::move(owner)), kind(kind), bytes(bytes) {}
    ~Lease() { owner->release(kind, bytes); }
  };
  explicit Impl(const Limits& limits) : limits(limits) {}
  std::shared_ptr<void> acquire(Kind kind, size_t charge = 0) {
    std::lock_guard<std::mutex> guard(mutex);
    auto* count = kind == Channel ? &channels : kind == Setup ? &setups : kind == Monitor ? &monitors : kind == Operation ? &operations : nullptr;
    const auto max = kind == Channel ? limits.maxChannels : kind == Setup ? limits.maxSetups : kind == Monitor ? limits.maxMonitors : limits.maxInFlight;
    if ((count && *count >= max) || charge > limits.maxInFlightBytes - bytes) return {};
    auto lease = std::make_shared<Lease>(shared_from_this(), kind, charge);
    if (count) ++*count;
    bytes += charge;
    return lease;
  }
  void release(Kind kind, size_t charge) {
    std::lock_guard<std::mutex> guard(mutex);
    auto* count = kind == Channel ? &channels : kind == Setup ? &setups : kind == Monitor ? &monitors : kind == Operation ? &operations : nullptr;
    if (count) --*count;
    bytes -= charge;
  }
};
GatewayPV::Budget::Budget(const Limits& limits) : impl(std::make_shared<Impl>(limits)) {}

struct GatewayPV::State : std::enable_shared_from_this<GatewayPV::State> {
  struct Session;
  struct Setup;
  struct Stream;
  struct Pending;
  pvxs::client::Context context;
  const std::string upstream, server;
  const Limits limits;
  const std::shared_ptr<Budget> budget;
  std::atomic<bool> alive{true};
  mutable std::mutex mutex;
  std::map<void*, std::shared_ptr<Session>> sessions;
  std::map<void*, std::shared_ptr<Setup>> setups;
  std::map<void*, std::shared_ptr<Stream>> streams;
  std::map<void*, std::shared_ptr<Pending>> pending;
  size_t inFlightBytes = 0;
  std::atomic<uint64_t> connections{0}, operations{0}, completed{0}, failed{0}, rejected{0}, uncertain{0};

  State(pvxs::client::Context context, std::string upstream, std::string server, Limits limits, std::shared_ptr<Budget> budget)
      : context(std::move(context)), upstream(std::move(upstream)), server(std::move(server)), limits(limits), budget(std::move(budget)) {}
  std::shared_ptr<Pending> admit(Exec&& target, pvxs::Value value, bool mutation,
                                pvxs::ProxyAction action = pvxs::ProxyAction::Process,
                                const std::shared_ptr<Setup>& setup = {}, const std::shared_ptr<Session>& session = {});
  void rpc(Exec&& target, pvxs::Value value, const std::shared_ptr<Session>& session);
  void release(Pending* work);
  void cancelSetup(Pending* work);
  void invalidateSession(Pending* work);
  void setup(std::unique_ptr<pvxs::server::ConnectOp>&& target, const std::shared_ptr<Session>& session);
  void setupProxy(std::unique_ptr<pvxs::server::ProxySetupOp>&& target, const std::shared_ptr<Session>& session);
  void subscribe(std::unique_ptr<pvxs::server::MonitorSetupOp>&& target, const std::shared_ptr<Session>& session);
};

struct GatewayPV::State::Pending : std::enable_shared_from_this<Pending> {
  std::shared_ptr<State> state;
  const size_t bytes;
  const bool mutation;
  const pvxs::ProxyAction action;
  const Clock::time_point deadline;
  std::mutex mutex;
  std::shared_ptr<pvxs::server::ExecOp> target;
  std::weak_ptr<Setup> setup;
  std::weak_ptr<Session> session;
  std::shared_ptr<void> budgetLease;
  pvxs::Value value, request;
  std::shared_ptr<pvxs::client::Operation> operation;
  pvxs::Timer timer;
  bool done = false, dispatched = false;
  Pending(std::shared_ptr<State> state, Exec&& target, pvxs::Value value, size_t bytes,
          bool mutation, pvxs::ProxyAction action)
      : state(std::move(state)), bytes(bytes), mutation(mutation), action(action),
        deadline(Clock::now() + std::chrono::milliseconds(this->state->limits.timeoutMs)),
        target(std::move(target)), value(std::move(value)), request(this->target->pvRequest()) {}

  void arm() {
    const auto reply = target;
    auto weak = std::weak_ptr<Pending>(shared_from_this());
    reply->onCancel([weak] { if (auto self = weak.lock()) self->finish({}, "downstream cancelled", true); });
    timer = reply->timerOneShot(state->limits.timeoutMs / 1000.0,
        [weak] { if (auto self = weak.lock()) self->finish({}, "gateway operation deadline exceeded", false, true); });
  }
  void handle(std::shared_ptr<pvxs::client::Operation> op) {
    std::lock_guard<std::mutex> guard(mutex);
    if (!done) operation = std::move(op);
  }
  void commit(bool preliminary = false) {
    // The commitment point is the transition to dispatched under the source,
    // cancellation, registration and access-policy fences. Every lock acquired
    // on this transport-loop path uses try_lock and contention fails closed.
    std::unique_lock<std::mutex> sourceFence(state->mutex, std::try_to_lock);
    std::unique_lock<std::mutex> operationFence(mutex, std::try_to_lock);
    if (!sourceFence.owns_lock() || !operationFence.owns_lock() || !state->alive || done ||
        dispatched || Clock::now() >= deadline || !target)
      throw std::runtime_error("gateway operation cancelled, retired, expired or already dispatched; not retried");
    const auto mark = [this, preliminary] { if (!preliminary) dispatched = true; return true; };
    if (mutation && !authorizeWriteCommit(*target, mark))
      throw std::runtime_error("access or retirement changed before upstream dispatch; not retried");
    if (!mutation) mark();
  }
  void result(pvxs::client::Result&& result) {
    try {
      const auto status = result.status();
      auto resultValue = result();
      try {
        if (resultValue) ValueCharge(state->limits.maxPayloadBytes).measure(resultValue);
        if (status.message.size() > state->limits.maxPayloadBytes || status.stack.size() > state->limits.maxPayloadBytes - status.message.size())
          throw std::runtime_error("upstream status exceeds byte limit");
      } catch (const std::exception& e) {
        finish({}, std::string("upstream completed; response rejected by gateway: ") + e.what()); return;
      }
      finish(std::move(resultValue), {}, false, false, status);
    } catch (const pvxs::client::RemoteError& e) {
      finish({}, e.what(), false, false, e.status());
    } catch (const pvxs::client::Disconnect& e) { finish({}, e.what(), false, true); }
    catch (const pvxs::client::Timeout& e) { finish({}, e.what(), false, true); }
    catch (const std::exception& e) { finish({}, e.what()); }
  }
  void finish(pvxs::Value result, std::string error, bool cancelled = false, bool transport = false,
              pvxs::RemoteStatus status = {}) {
    std::shared_ptr<pvxs::server::ExecOp> reply;
    std::shared_ptr<pvxs::client::Operation> previous;
    bool isUncertain = false;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (done) return;
      done = true;
      isUncertain = mutation && dispatched && (cancelled || transport);
      reply = std::move(target);
      previous = std::move(operation);
    }
    state->release(this);
    budgetLease.reset();
    if (isUncertain) {
      ++state->uncertain;
      error = "upstream outcome uncertain after dispatch; not retried: " + error;
    }
    if (cancelled || transport) state->cancelSetup(this);
    previous.reset(); // Handles were created with asynchronous implicit cancellation.

    if (cancelled) { ++state->failed; state->invalidateSession(this); return; }
    try {
      if (!error.empty()) {
        ++state->failed;
        if (status.isDefault()) status = pvxs::RemoteStatus(pvxs::RemoteStatus::Error, error);
        reply->error(status);
      }
      else {
        ++state->completed;
        if (action == pvxs::ProxyAction::ArrayGetLength) {
          auto* proxy = dynamic_cast<pvxs::server::ProxyExecOp*>(reply.get());
          if (!proxy) throw std::logic_error("missing array length reply handle");
          proxy->replyLength(result.as<uint64_t>(), status);
        } else if (result) reply->reply(result, status);
        else reply->reply(status);
      }
    } catch (...) { /* peer may have gone away concurrently with completion */ }
    if (transport) state->invalidateSession(this);
  }
};

struct GatewayPV::State::Session : std::enable_shared_from_this<Session> {
  std::weak_ptr<State> state;
  std::shared_ptr<pvxs::server::ChannelControl> channel;
  std::shared_ptr<pvxs::client::Connect> connection;
  std::shared_ptr<void> budgetLease;
  std::mutex lifecycle;
  std::atomic<bool> connected{false}, closed{false};
  Session(const std::shared_ptr<State>& state, Channel&& target) : state(state), channel(std::move(target)) {}
  void connectionEvent(bool value) {
    std::lock_guard<std::mutex> guard(lifecycle);
    if (closed || connected.exchange(value) == value) return;
    if (auto owner = state.lock()) {
      if (value) ++owner->connections;
      else --owner->connections;
    }
  }
  void close() {
    std::shared_ptr<pvxs::client::Connect> previous;
    {
      std::lock_guard<std::mutex> guard(lifecycle);
      if (closed.exchange(true)) return;
      if (connected.exchange(false)) if (auto owner = state.lock()) --owner->connections;
      previous = std::move(connection);
      budgetLease.reset();
    }
    if (auto owner = state.lock()) {
      std::lock_guard<std::mutex> guard(owner->mutex);
      owner->sessions.erase(this);
    }
  }
  void lost() {
    {
      std::lock_guard<std::mutex> guard(lifecycle);
      if (closed || !connected.exchange(false)) return;
    }
    auto owner = state.lock();
    if (!owner) return;
    --owner->connections;
    std::vector<std::shared_ptr<Pending>> outstanding;
    {
      std::lock_guard<std::mutex> guard(owner->mutex);
      for (const auto& entry : owner->pending)
        if (entry.second->session.lock().get() == this) outstanding.push_back(entry.second);
    }
    for (const auto& work : outstanding) work->finish({}, "upstream disconnected", false, true);
    channel->close();
    close();
  }
  void start() {
    auto owner = state.lock();
    if (!owner) return;
    auto weak = std::weak_ptr<Session>(shared_from_this());
    channel->onClose([weak](const std::string&) { if (auto self = weak.lock()) self->close(); });
    channel->onOp([weak](std::unique_ptr<pvxs::server::ConnectOp>&& op) {
      if (auto self = weak.lock()) if (auto state = self->state.lock()) { state->setup(std::move(op), self); return; }
      op->error("gateway endpoint retired");
    });
    channel->onProxy([weak](std::unique_ptr<pvxs::server::ProxySetupOp>&& op) {
      if (auto self = weak.lock()) if (auto state = self->state.lock()) { state->setupProxy(std::move(op), self); return; }
      op->error("gateway endpoint retired");
    });
    channel->onRPC([weak](Exec&& op, pvxs::Value&& value) {
      if (auto self = weak.lock()) if (auto state = self->state.lock()) {
        state->rpc(std::move(op), std::move(value), self); return;
      }
      op->error("gateway endpoint retired");
    });
    channel->onSubscribe([weak](std::unique_ptr<pvxs::server::MonitorSetupOp>&& op) {
      if (auto self = weak.lock()) if (auto state = self->state.lock()) { state->subscribe(std::move(op), self); return; }
      op->error("gateway endpoint retired");
    });
  }
  void connect() {
    auto owner = state.lock();
    if (!owner) return;
    auto weak = std::weak_ptr<Session>(shared_from_this());
    std::lock_guard<std::mutex> guard(lifecycle);
    if (closed) return;
    connection = owner->context.connect(owner->upstream).server(owner->server).syncCancel(false)
        .onConnect([weak] { if (auto self = weak.lock()) self->connectionEvent(true); })
        .onDisconnect([weak] { if (auto self = weak.lock()) self->lost(); }).exec();
  }
};

void GatewayPV::State::invalidateSession(Pending* work) {
  if (auto session = work->session.lock()) {
    // ExecOp replies enqueue on the local server loop. Closing after that
    // enqueue preserves the error before channel destruction, then forces a
    // fresh native INIT instead of leaving a reusable but dead downstream IOID.
    session->channel->closeAsync();
  }
}

struct GatewayPV::State::Setup : std::enable_shared_from_this<Setup> {
  std::weak_ptr<State> state;
  std::weak_ptr<Session> session;
  std::shared_ptr<pvxs::server::ConnectOp> target;
  std::shared_ptr<pvxs::server::ProxySetupOp> proxy;
  std::shared_ptr<pvxs::client::Operation> operation;
  std::shared_ptr<Pending> current;
  std::mutex mutex;
  bool connected = false, closed = false, haveType = false, preflight = false;
  pvxs::ProxyPrototype prototype;
  pvxs::RemoteStatus initStatus;
  const Clock::time_point deadline = Clock::now();
  std::shared_ptr<void> budgetLease;
  Setup(const std::shared_ptr<State>& owner, const std::shared_ptr<Session>& session) : state(owner), session(session) {}
  void close(const std::string& error = {}, pvxs::RemoteStatus status = {}) {
    std::shared_ptr<pvxs::server::ConnectOp> reply;
    std::shared_ptr<pvxs::server::ProxySetupOp> proxyReply;
    std::shared_ptr<pvxs::client::Operation> previous;
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (closed) return;
      closed = true;
      if (!connected) { reply = target; proxyReply = proxy; }
      previous = std::move(operation); pending = current;
    }
    if (pending) pending->finish({}, error.empty() ? "downstream operation closed" : error, true, true);
    budgetLease.reset();
    if (auto owner = state.lock()) { std::lock_guard<std::mutex> guard(owner->mutex); owner->setups.erase(this); }
    if (!error.empty()) {
      if (status.isDefault()) status = pvxs::RemoteStatus(pvxs::RemoteStatus::Error, error);
      if (reply) reply->error(status);
      if (proxyReply) proxyReply->error(status);
    }
  }
  void publishReady() {
    std::shared_ptr<pvxs::server::ConnectOp> reply;
    std::shared_ptr<pvxs::server::ProxySetupOp> proxyReply;
    pvxs::ProxyPrototype type;
    pvxs::RemoteStatus status;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (closed || connected || !operation || !haveType) return;
      auto owner = state.lock();
      if (!owner || !owner->alive) return;
      connected = true; reply = target; proxyReply = proxy; type = prototype; status = initStatus;
    }
    try { if (reply) reply->connect(type.get, status); else proxyReply->connect(type, status); }
    catch (const std::exception& e) { close(e.what()); }
  }
  void handle(std::shared_ptr<pvxs::client::Operation> value) {
    { std::lock_guard<std::mutex> guard(mutex); if (closed) return; operation = std::move(value); }
    publishReady();
  }
  void ready(const pvxs::Value& type, const pvxs::RemoteStatus& status = {}) {
    try { if (auto owner = state.lock()) ValueCharge(owner->limits.maxPayloadBytes).measure(type); }
    catch (const std::exception& e) { close(e.what()); return; }
    { std::lock_guard<std::mutex> guard(mutex); if (closed) return; prototype.get = prototype.put = type; initStatus = status; haveType = true; }
    publishReady();
  }
  void ready(const pvxs::ProxyPrototype& type, const pvxs::RemoteStatus& status = {}) {
    try { if (auto owner = state.lock()) {
      ValueCharge charge(owner->limits.maxPayloadBytes); charge.measure(type.put); charge.measure(type.get);
    } }
    catch (const std::exception& e) { close(e.what()); return; }
    { std::lock_guard<std::mutex> guard(mutex); if (closed) return; prototype = type; initStatus = status; haveType = true; }
    publishReady();
  }
  void beforeSend() {
    std::shared_ptr<Pending> work;
    bool preliminary;
    {
      std::unique_lock<std::mutex> guard(mutex, std::try_to_lock);
      if (!guard.owns_lock() || closed || !current) throw std::runtime_error("gateway operation unavailable before dispatch; not retried");
      work = current; preliminary = preflight;
    }
    work->commit(preliminary);
  }
  size_t arrayWidth() {
    std::lock_guard<std::mutex> guard(mutex);
    const auto type = prototype.get.type();
    if (!type.isarray()) return 1;
    if (type == pvxs::TypeCode::StringA) return sizeof(std::string);
    if (type.kind() == pvxs::Kind::Compound) return sizeof(pvxs::Value) + 32u;
    return type.scalarOf().size();
  }
  void dispatch(Exec&& target, pvxs::Value value, bool mutation, bool write,
                pvxs::ProxyRequest* request = nullptr) {
    auto owner = state.lock();
    if (!owner) { target->error("gateway endpoint retired"); return; }
    auto work = owner->admit(std::move(target), std::move(value), mutation,
        request ? request->action : pvxs::ProxyAction::Process, shared_from_this(), session.lock());
    if (!work) return;
    std::shared_ptr<pvxs::client::Operation> handle;
    {
      std::lock_guard<std::mutex> guard(mutex);
      std::lock_guard<std::mutex> workGuard(work->mutex);
      if (!closed && connected && operation && !current && !work->done) {
        current = work; handle = operation; preflight = request && request->action == pvxs::ProxyAction::ArrayGet && !request->count;
      }
    }
    if (!handle) { work->finish({}, "gateway operation closed or already executing"); return; }
    work->handle(handle);
    auto callback = [weak = std::weak_ptr<Pending>(work)](pvxs::client::Result&& result) {
      if (auto work = weak.lock()) work->result(std::move(result));
    };
    try {
      if (request) {
        const auto width = arrayWidth();
        if (request->action == pvxs::ProxyAction::ArrayGet || request->action == pvxs::ProxyAction::ArrayPut ||
            request->action == pvxs::ProxyAction::ArraySetLength) {
          if (request->action != pvxs::ProxyAction::ArraySetLength && !request->stride)
            throw std::runtime_error("gateway ARRAY stride must be positive");
          const auto count = request->action == pvxs::ProxyAction::ArraySetLength ? request->length : request->count;
          if (count > owner->limits.maxPayloadBytes / width)
            throw std::runtime_error("gateway ARRAY request exceeds payload limit");
          if (request->action != pvxs::ProxyAction::ArraySetLength && count &&
              count - 1 > (UINT64_MAX - request->offset) / request->stride)
            throw std::runtime_error("gateway ARRAY offset/stride range overflows");
        }
        if (request->action == pvxs::ProxyAction::ArrayGet && !request->count) {
          auto original = *request;
          pvxs::ProxyRequest length; length.action = pvxs::ProxyAction::ArrayGetLength;
          auto weak = std::weak_ptr<Setup>(shared_from_this());
          handle->execute(length, [weak, work, handle, original, callback, width](pvxs::client::Result&& result) mutable {
            try {
              const auto length = result().as<uint64_t>();
              const auto count = original.offset >= length ? 0 : (length - 1 - original.offset) / original.stride + 1;
              if (count > work->state->limits.maxPayloadBytes / width)
                throw std::runtime_error("gateway ARRAY count=0 result span exceeds payload limit");
              auto setup = weak.lock();
              if (!setup) throw std::runtime_error("gateway operation closed during ARRAY length query");
              { std::lock_guard<std::mutex> guard(setup->mutex); setup->preflight = false; }
              handle->execute(original, callback);
            } catch (const pvxs::client::RemoteError& e) { work->finish({}, e.what(), false, false, e.status()); }
            catch (const std::exception& e) { work->finish({}, e.what()); }
          });
        } else handle->execute(*request, callback);
      }
      else if (write) handle->reExecPut(work->value, callback);
      else handle->reExecGet(callback);
    } catch (const std::exception& e) { work->finish({}, e.what()); }
  }
  void result(pvxs::client::Result&& result, bool info) {
    try {
      auto value = result();
      if (info) {
        if (auto owner = state.lock()) ValueCharge(owner->limits.maxPayloadBytes).measure(value);
        // GET_FIELD has no separate execution phase or persistent type state.
        std::shared_ptr<pvxs::server::ConnectOp> reply;
        { std::lock_guard<std::mutex> guard(mutex); if (closed) return; connected = true; reply = target; }
        reply->connect(value, result.status()); close();
      }
    } catch (const pvxs::client::RemoteError& e) { close(e.what(), e.status()); }
    catch (const std::exception& e) { close(e.what()); }
  }
};

void GatewayPV::State::release(Pending* work) {
  {
    std::lock_guard<std::mutex> guard(mutex);
    const auto found = pending.find(work);
    if (found != pending.end()) { inFlightBytes -= work->bytes; pending.erase(found); }
  }
  if (auto setup = work->setup.lock()) {
    std::lock_guard<std::mutex> guard(setup->mutex);
    if (setup->current.get() == work) setup->current.reset();
  }
}

void GatewayPV::State::cancelSetup(Pending* work) {
  if (auto setup = work->setup.lock()) setup->close();
}

struct GatewayPV::State::Stream : std::enable_shared_from_this<Stream> {
  std::weak_ptr<State> state;
  std::weak_ptr<Session> session;
  std::shared_ptr<pvxs::server::MonitorSetupOp> setup;
  std::shared_ptr<pvxs::server::MonitorControlOp> control;
  std::shared_ptr<pvxs::client::Subscription> subscription;
  std::mutex mutex;
  bool closed = false, connecting = false, running = false;
  uint64_t startGeneration = 0;
  const Clock::time_point deadline = Clock::now();
  std::shared_ptr<void> budgetLease;
  Stream(const std::shared_ptr<State>& owner, const std::shared_ptr<Session>& session,
         std::unique_ptr<pvxs::server::MonitorSetupOp>&& setup)
      : state(owner), session(session), setup(std::move(setup)) {}
  void close(const std::string& error = {}, pvxs::RemoteStatus status = {}) {
    std::shared_ptr<pvxs::server::MonitorSetupOp> setupReply;
    std::shared_ptr<pvxs::server::MonitorControlOp> final;
    std::shared_ptr<pvxs::client::Subscription> previous;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (closed) return;
      closed = true;
      setupReply = std::move(setup); final = std::move(control); previous = std::move(subscription);
    }
    if (auto owner = state.lock()) { std::lock_guard<std::mutex> guard(owner->mutex); owner->streams.erase(this); }
    budgetLease.reset();
    if (!error.empty() && status.isDefault()) status = pvxs::RemoteStatus(pvxs::RemoteStatus::Error, error);
    if (setupReply && !final && !error.empty()) setupReply->error(status);
    if (final) { if (!error.empty()) final->error(status); else final->finish(status); }
  }
  void start(bool value) {
    std::lock_guard<std::mutex> guard(mutex);
    if (closed) return;
    running = value; ++startGeneration;
    if (subscription) subscription->pauseAsync(!value);
  }
  void handle(std::shared_ptr<pvxs::client::Subscription> value) {
    std::lock_guard<std::mutex> guard(mutex);
    if (closed) return;
    subscription = std::move(value);
    subscription->pauseAsync(!running);
  }
  void ready(const pvxs::Value& type, const pvxs::RemoteStatus& status = {}) {
    try { if (auto owner = state.lock()) ValueCharge(owner->limits.maxPayloadBytes).measure(type); }
    catch (const std::exception& e) { close(e.what()); return; }
    std::shared_ptr<pvxs::server::MonitorSetupOp> reply;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (closed || control || connecting) return;
      auto owner = state.lock(); if (!owner || !owner->alive) return;
      connecting = true; reply = setup;
    }
    std::shared_ptr<pvxs::server::MonitorControlOp> next;
    try { next = reply->connect(type, status); }
    catch (const std::exception& e) { close(e.what()); return; }
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (!closed) control = next;
    }
    auto weak = std::weak_ptr<Stream>(shared_from_this());
    uint64_t generation;
    { std::lock_guard<std::mutex> guard(mutex); generation = startGeneration; }
    next->onStart([weak](bool start) { if (auto stream = weak.lock()) stream->start(start); });
    // connect() publishes INIT before callback installation. A fast peer may
    // already have sent START. Reconcile that current state unless a newer
    // transition reached our installed handler while stats were being read.
    pvxs::server::MonitorStat stats;
    next->stats(stats);
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (!closed && generation == startGeneration) {
        running = stats.running; ++startGeneration;
        if (subscription) subscription->pauseAsync(!running);
      }
    }
  }
  void event(pvxs::client::Subscription& sub) {
    for (;;) {
      try {
        auto event = sub.popEvent();
        if (!event.value) return;
        if (auto owner = state.lock()) {
          const auto bytes = ValueCharge(owner->limits.maxPayloadBytes).measure(event.value);
          event.lease = owner->budget->impl->acquire(Budget::Impl::MonitorValue, bytes);
          if (!event.lease) throw std::runtime_error("gateway global monitor byte capacity exhausted");
        }
        std::shared_ptr<pvxs::server::MonitorControlOp> next;
        { std::lock_guard<std::mutex> guard(mutex); if (closed) return; next = control; }
        if (next) next->postEvent(event);
      } catch (const pvxs::client::Connected&) { continue; }
      catch (const pvxs::client::Finished& e) { close({}, e.status()); return; }
      catch (const pvxs::client::Disconnect&) { close("upstream monitor disconnected"); return; }
      catch (const pvxs::client::RemoteError& e) { close(e.what(), e.status()); return; }
      catch (const std::exception& e) { close(e.what()); return; }
    }
  }
};

std::shared_ptr<GatewayPV::State::Pending> GatewayPV::State::admit(Exec&& target, pvxs::Value value,
    bool mutation, pvxs::ProxyAction action, const std::shared_ptr<Setup>& setup,
    const std::shared_ptr<Session>& session) {
  size_t bytes;
  try { bytes = ValueCharge(limits.maxPayloadBytes).measure(value) + ValueCharge(limits.maxPayloadBytes).measure(target->pvRequest()); }
  catch (const std::exception& e) { ++rejected; target->error(e.what()); return {}; }
  auto lease = budget->impl->acquire(Budget::Impl::Operation, bytes);
  if (!lease) { ++rejected; target->error("gateway global operation capacity exhausted"); return {}; }
  auto result = std::make_shared<Pending>(shared_from_this(), std::move(target), std::move(value), bytes, mutation, action);
  result->budgetLease = std::move(lease);
  result->setup = setup; result->session = session;
  result->arm(); // Initialize callbacks before publication makes retirement possible.
  bool accepted = false;
  {
    std::lock_guard<std::mutex> guard(mutex);
    std::lock_guard<std::mutex> operationGuard(result->mutex);
    if (alive && !result->done && pending.size() < limits.maxInFlight && bytes <= limits.maxInFlightBytes - inFlightBytes) {
      pending.emplace(result.get(), result); inFlightBytes += bytes; ++operations; accepted = true;
    }
  }
  if (!accepted) { ++rejected; result->finish({}, "gateway retired or operation capacity exhausted"); return {}; }
  return result;
}

void GatewayPV::State::rpc(Exec&& target, pvxs::Value value, const std::shared_ptr<Session>& session) {
  auto work = admit(std::move(target), std::move(value), true, pvxs::ProxyAction::Process, {}, session);
  if (!work) return;
  auto weak = std::weak_ptr<Pending>(work);
  try {
    work->handle(context.rpc(upstream, work->value).server(server).rawRequest(work->request).syncCancel(false)
        .beforeSend([weak] {
          auto work = weak.lock();
          if (!work) throw std::runtime_error("gateway operation cancelled before dispatch");
          work->commit();
        }).result([weak](pvxs::client::Result&& result) { if (auto work = weak.lock()) work->result(std::move(result)); }).exec());
  } catch (const std::exception& e) { work->finish({}, e.what()); }
}

void GatewayPV::State::setup(std::unique_ptr<pvxs::server::ConnectOp>&& target,
                             const std::shared_ptr<Session>& session) {
  try { ValueCharge(limits.maxPayloadBytes).measure(target->pvRequest()); }
  catch (const std::exception& e) { ++rejected; target->error(e.what()); return; }
  auto lease = budget->impl->acquire(Budget::Impl::Setup, ValueCharge(limits.maxPayloadBytes).measure(target->pvRequest()));
  if (!lease) { ++rejected; target->error("gateway global setup capacity exhausted"); return; }
  auto work = std::make_shared<Setup>(shared_from_this(), session);
  work->budgetLease = std::move(lease);
  work->target = std::move(target);
  auto weak = std::weak_ptr<Setup>(work);
  const auto op = work->target;
  const auto request = op->pvRequest();
  const auto kind = op->op();
  const auto field = op->subField();
  op->onClose([weak](const std::string&) { if (auto work = weak.lock()) work->close(); });
  op->onGet([weak, mutation = processingGet(request)](Exec&& exec) {
    if (auto work = weak.lock()) work->dispatch(std::move(exec), {}, mutation, false);
    else exec->error("gateway endpoint retired");
  });
  op->onPut([weak](Exec&& exec, pvxs::Value&& value) {
    if (auto work = weak.lock()) work->dispatch(std::move(exec), std::move(value), true, true);
    else exec->error("gateway endpoint retired");
  });
  {
    std::lock_guard<std::mutex> guard(mutex);
    if (!alive || setups.size() >= limits.maxSetups) { ++rejected; op->error("gateway setup capacity exhausted"); return; }
    setups.emplace(work.get(), work);
  }
  const auto ready = [weak](const pvxs::Value& value, const pvxs::RemoteStatus& status) { if (auto work = weak.lock()) work->ready(value, status); };
  const auto commit = [weak] {
    auto work = weak.lock();
    if (!work) throw std::runtime_error("gateway operation closed before dispatch");
    work->beforeSend();
  };
  const auto result = [weak, info = kind == pvxs::server::OpBase::Info](pvxs::client::Result&& result) {
    if (auto work = weak.lock()) work->result(std::move(result), info);
  };
  try {
    if (kind == pvxs::server::OpBase::Info)
      work->handle(context.info(upstream).server(server).subField(field).rawRequest(request).syncCancel(false).result(result).exec());
    else if (kind == pvxs::server::OpBase::Get)
      work->handle(context.get(upstream).server(server).rawRequest(request).autoExec(false).syncCancel(false).onInit(ready).beforeSend(commit).result(result).exec());
    else if (kind == pvxs::server::OpBase::Put)
      work->handle(context.put(upstream).server(server).rawRequest(request).autoExec(false).fetchPresent(false).syncCancel(false).onInit(ready).beforeSend(commit).result(result).exec());
    else throw std::runtime_error("unexpected gateway operation kind");
  } catch (const std::exception& e) { work->close(e.what()); }
}

void GatewayPV::State::setupProxy(std::unique_ptr<pvxs::server::ProxySetupOp>&& target,
                                  const std::shared_ptr<Session>& session) {
  try { ValueCharge(limits.maxPayloadBytes).measure(target->pvRequest()); }
  catch (const std::exception& e) { ++rejected; target->error(e.what()); return; }
  auto lease = budget->impl->acquire(Budget::Impl::Setup, ValueCharge(limits.maxPayloadBytes).measure(target->pvRequest()));
  if (!lease) { ++rejected; target->error("gateway global setup capacity exhausted"); return; }
  auto work = std::make_shared<Setup>(shared_from_this(), session);
  work->budgetLease = std::move(lease);
  work->proxy = std::move(target);
  auto weak = std::weak_ptr<Setup>(work);
  const auto op = work->proxy;
  const auto kind = op->kind();
  const auto request = op->pvRequest();
  op->onClose([weak](const std::string&) { if (auto work = weak.lock()) work->close(); });
  op->onExec([weak](std::unique_ptr<pvxs::server::ProxyExecOp>&& op, pvxs::ProxyRequest&& request) {
    if (auto work = weak.lock()) work->dispatch(std::move(op), request.value, request.mutating(), false, &request);
    else op->error("gateway endpoint retired");
  });
  {
    std::lock_guard<std::mutex> guard(mutex);
    if (!alive || setups.size() >= limits.maxSetups) { ++rejected; op->error("gateway setup capacity exhausted"); return; }
    setups.emplace(work.get(), work);
  }
  try {
    work->handle(context.proxy(upstream, kind).server(server).rawRequest(request).syncCancel(false)
        .onInit([weak](const pvxs::ProxyPrototype& value, const pvxs::RemoteStatus& status) { if (auto work = weak.lock()) work->ready(value, status); })
        .beforeSend([weak] {
          auto work = weak.lock();
          if (!work) throw std::runtime_error("gateway operation closed before dispatch");
          work->beforeSend();
        }).result([weak](pvxs::client::Result&& result) { if (auto work = weak.lock()) work->result(std::move(result), false); }).exec());
  } catch (const std::exception& e) { work->close(e.what()); }
}

void GatewayPV::State::subscribe(std::unique_ptr<pvxs::server::MonitorSetupOp>&& target,
                                 const std::shared_ptr<Session>& session) {
  const auto request = target->pvRequest();
  try {
    uint64_t queue = 0;
    if (request["record._options.queueSize"] &&
        (!request["record._options.queueSize"].as(queue) || queue > limits.maxMonitorQueue))
      throw std::runtime_error("gateway monitor queue exceeds limit");
    ValueCharge(limits.maxPayloadBytes).measure(request);
  } catch (const std::exception& e) { ++rejected; target->error(e.what()); return; }
  auto lease = budget->impl->acquire(Budget::Impl::Monitor, ValueCharge(limits.maxPayloadBytes).measure(request));
  if (!lease) { ++rejected; target->error("gateway global monitor capacity exhausted"); return; }
  auto stream = std::make_shared<Stream>(shared_from_this(), session, std::move(target));
  stream->budgetLease = std::move(lease);
  auto weak = std::weak_ptr<Stream>(stream);
  const auto setup = stream->setup;
  setup->onClose([weak](const std::string&) { if (auto stream = weak.lock()) stream->close(); });
  {
    std::lock_guard<std::mutex> guard(mutex);
    if (!alive || streams.size() >= limits.maxMonitors) { ++rejected; setup->error("gateway monitor capacity exhausted"); return; }
    streams.emplace(stream.get(), stream);
  }
  try {
    auto subscription = context.monitor(upstream).server(server).rawRequest(request).syncCancel(false).autoExec(false)
        .maskConnected(true).maskDisconnected(false)
        .onInit([weak](pvxs::client::Subscription&, const pvxs::Value& value, const pvxs::RemoteStatus& status) { if (auto stream = weak.lock()) stream->ready(value, status); })
        .event([weak](pvxs::client::Subscription& sub) { if (auto stream = weak.lock()) stream->event(sub); }).exec();
    stream->handle(std::move(subscription));
  } catch (const std::exception& e) { stream->close(e.what()); }
}

GatewayPV::GatewayPV(std::shared_ptr<State> state) : state_(std::move(state)) {}
std::shared_ptr<GatewayPV> GatewayPV::create(pvxs::client::Context context, std::string name, std::string server) {
  return create(std::move(context), std::move(name), std::move(server), Limits{});
}
std::shared_ptr<GatewayPV> GatewayPV::create(pvxs::client::Context context, std::string name,
                                           std::string server, const Limits& limits) {
  return create(std::move(context), std::move(name), std::move(server), limits, std::make_shared<Budget>(limits));
}
std::shared_ptr<GatewayPV> GatewayPV::create(pvxs::client::Context context, std::string name,
                                           std::string server, const Limits& limits, std::shared_ptr<Budget> budget) {
  if (!budget) throw std::invalid_argument("gateway shared budget is required");
  if (name.empty() || name.find('\0') != std::string::npos || !validServer(server))
    throw std::invalid_argument("gateway requires a name and a unicast IPv4 TCP server");
  if (!limits.maxChannels || !limits.maxSetups || !limits.maxInFlight || !limits.maxMonitors ||
      !limits.maxPayloadBytes || !limits.maxInFlightBytes || !limits.maxMonitorQueue || !limits.timeoutMs)
    throw std::invalid_argument("gateway limits must be positive");
  if (pvxs::xrmProxyApiVersion() != 1) throw std::runtime_error("PVXS proxy API unavailable");
  return std::shared_ptr<GatewayPV>(new GatewayPV(std::make_shared<State>(std::move(context), std::move(name), std::move(server), limits, std::move(budget))));
}
GatewayPV::~GatewayPV() { deactivate(); }
void GatewayPV::attach(Channel&& channel) {
  auto lease = state_->budget->impl->acquire(Budget::Impl::Channel);
  if (!lease) { ++state_->rejected; channel->close(); return; }
  auto session = std::make_shared<State::Session>(state_, std::move(channel));
  session->budgetLease = std::move(lease);
  session->start(); // Install local callbacks before retirement can find this session.
  bool accepted = false;
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (state_->alive && !session->closed && state_->sessions.size() < state_->limits.maxChannels) {
      state_->sessions.emplace(session.get(), session);
      accepted = true;
    }
  }
  if (!accepted) { ++state_->rejected; session->channel->close(); session->close(); return; }
  session->connect();
}
void GatewayPV::deactivate() {
  std::vector<std::shared_ptr<State::Pending>> pending;
  std::vector<std::shared_ptr<State::Setup>> setups;
  std::vector<std::shared_ptr<State::Stream>> streams;
  std::vector<std::shared_ptr<State::Session>> sessions;
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (!state_->alive.exchange(false)) return;
    for (const auto& item : state_->pending) pending.push_back(item.second);
    for (const auto& item : state_->setups) setups.push_back(item.second);
    for (const auto& item : state_->streams) streams.push_back(item.second);
    for (const auto& item : state_->sessions) sessions.push_back(item.second);
  }
  // Complete requests before channel closure so a committed mutation receives
  // an uncertainty error instead of silently entering downstream reconnect.
  for (const auto& item : pending) item->finish({}, "gateway endpoint retired", false, true);
  for (const auto& item : setups) item->close("gateway endpoint retired");
  for (const auto& item : streams) item->close("gateway endpoint retired");
  for (const auto& item : sessions) { item->channel->close(); item->close(); }
}
void GatewayPV::pump() {
  std::vector<std::shared_ptr<State::Setup>> expired;
  std::vector<std::shared_ptr<State::Stream>> expiredStreams;
  bool idle;
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    const auto now = Clock::now();
    for (const auto& entry : state_->setups)
      if (now - entry.second->deadline >= std::chrono::milliseconds(state_->limits.timeoutMs)) expired.push_back(entry.second);
    for (const auto& entry : state_->streams)
      if (now - entry.second->deadline >= std::chrono::milliseconds(state_->limits.timeoutMs)) expiredStreams.push_back(entry.second);
    idle = state_->sessions.empty() && state_->setups.empty() && state_->streams.empty() && state_->pending.empty();
  }
  for (const auto& setup : expired) {
    bool connected;
    { std::lock_guard<std::mutex> guard(setup->mutex); connected = setup->connected; }
    if (!connected) setup->close("gateway setup deadline exceeded");
  }
  for (const auto& stream : expiredStreams) {
    bool connected;
    { std::lock_guard<std::mutex> guard(stream->mutex); connected = bool(stream->control); }
    if (!connected) stream->close("gateway monitor setup deadline exceeded");
  }
  if (idle && state_->alive) state_->context.cacheClear(state_->upstream, pvxs::client::Context::Clean);
}
bool GatewayPV::connected() const { return state_->alive && state_->connections != 0; }
GatewayPV::Status GatewayPV::status() const {
  Status result;
  std::lock_guard<std::mutex> guard(state_->mutex);
  result.connected = connected(); result.channels = state_->sessions.size();
  result.operations = state_->operations; result.monitors = state_->streams.size(); result.inFlight = state_->pending.size();
  result.completed = state_->completed; result.failed = state_->failed; result.rejected = state_->rejected; result.uncertain = state_->uncertain;
  return result;
}
} // namespace redis_pvxs_ioc
