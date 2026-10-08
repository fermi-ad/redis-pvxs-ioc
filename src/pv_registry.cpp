#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include "redis_pvxs_ioc/pv_registry.h"
#include "redis_pvxs_ioc/access_control.h"
#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace redis_pvxs_ioc {
namespace {
struct Registration {
  PVBinding binding;
  PVRegistry::Wrapper wrapper;
  std::atomic<bool> active{true};
  std::mutex mutex;
  std::vector<std::weak_ptr<pvxs::server::ChannelControl>> channels;
  void track(const std::shared_ptr<pvxs::server::ChannelControl>& channel) {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto it = channels.begin(); it != channels.end();) {
      if (it->expired()) it = channels.erase(it);
      else ++it;
    }
    channels.emplace_back(channel);
  }
  void close() {
    decltype(channels) previous;
    {
      std::lock_guard<std::mutex> guard(mutex);
      previous.swap(channels);
    }
    for (const auto& weak : previous) if (const auto channel = weak.lock()) channel->close();
  }
};

class TrackedExec final : public pvxs::server::ExecOp, public OperationAuthorization {
public:
  TrackedExec(std::unique_ptr<pvxs::server::ExecOp> target, std::shared_ptr<Registration> registration)
      : ExecOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)), registration_(std::move(registration)) {}
  void reply() override { target_->reply(); }
  void reply(const pvxs::Value& value) override { target_->reply(value); }
  void error(const std::string& message) override { target_->error(message); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onCancel(std::function<void()>&& fn) override { target_->onCancel(std::move(fn)); }
  bool authorized(const pvxs::Value& value) override {
    return registration_->active && authorizeWriteDispatch(*target_, value);
  }
private:
  pvxs::Timer _timerOneShot(double delay, std::function<void()>&& fn) override {
    return target_->timerOneShot(delay, std::move(fn));
  }
  std::unique_ptr<pvxs::server::ExecOp> target_;
  std::shared_ptr<Registration> registration_;
};

class TrackedConnect final : public pvxs::server::ConnectOp {
public:
  TrackedConnect(std::unique_ptr<pvxs::server::ConnectOp> target, std::shared_ptr<Registration> registration)
      : ConnectOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)), registration_(std::move(registration)) {}
  void connect(const pvxs::Value& type) override { target_->connect(type); }
  void error(const std::string& message) override { target_->error(message); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }
  void onGet(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&)>&& fn) override {
    auto registration = registration_;
    target_->onGet([registration, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op) {
      if (!registration->active) { op->error("endpoint retired"); return; }
      fn(std::move(op));
    });
  }
  void onPut(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    auto registration = registration_;
    target_->onPut([registration, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
      if (!registration->active) { op->error("endpoint retired"); return; }
      fn(std::make_unique<TrackedExec>(std::move(op), registration), std::move(value));
    });
  }
private:
  std::unique_ptr<pvxs::server::ConnectOp> target_;
  std::shared_ptr<Registration> registration_;
};

class TrackedChannel final : public pvxs::server::ChannelControl {
public:
  TrackedChannel(std::shared_ptr<pvxs::server::ChannelControl> target, std::shared_ptr<Registration> registration)
      : ChannelControl(target->name(), target->credentials(), target->op()),
        target_(std::move(target)), registration_(std::move(registration)) {
    registration_->track(target_);
  }
  void onOp(std::function<void(std::unique_ptr<pvxs::server::ConnectOp>&&)>&& fn) override {
    auto registration = registration_;
    target_->onOp([registration, fn = std::move(fn)](std::unique_ptr<pvxs::server::ConnectOp>&& op) {
      if (!registration->active) { op->error("endpoint retired"); return; }
      fn(std::make_unique<TrackedConnect>(std::move(op), registration));
    });
  }
  void onRPC(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    auto registration = registration_;
    target_->onRPC([registration, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
      if (!registration->active) { op->error("endpoint retired"); return; }
      fn(std::make_unique<TrackedExec>(std::move(op), registration), std::move(value));
    });
  }
  void onSubscribe(std::function<void(std::unique_ptr<pvxs::server::MonitorSetupOp>&&)>&& fn) override {
    auto registration = registration_;
    target_->onSubscribe([registration, fn = std::move(fn)](std::unique_ptr<pvxs::server::MonitorSetupOp>&& op) {
      if (!registration->active) { op->error("endpoint retired"); return; }
      fn(std::move(op));
    });
  }
  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }
  void close() override { target_->close(); }
private:
  void _updateInfo(const std::shared_ptr<const pvxs::server::ReportInfo>& info) override { target_->updateInfo(info); }
  std::shared_ptr<pvxs::server::ChannelControl> target_;
  std::shared_ptr<Registration> registration_;
};

struct Snapshot {
  std::map<std::string, std::shared_ptr<Registration>> entries;
  std::shared_ptr<const std::set<std::string>> names;
};
}

struct PVRegistry::Impl {
  // Serialize publishers, including the last fallible commit action. Readers
  // use immutable snapshots and never wait for policy DNS or runtime dispatch.
  std::mutex publishMutex;
  std::shared_ptr<const Snapshot> current = std::make_shared<Snapshot>();
};
struct PVRegistry::Prepared {
  std::shared_ptr<const Snapshot> previous, next;
  std::vector<std::shared_ptr<Registration>> retiring;
};

PVRegistry::PVRegistry() : impl_(std::make_unique<Impl>()) {}
PVRegistry::~PVRegistry() = default;
PVBindings PVRegistry::bindings() const {
  const auto snapshot = std::atomic_load(&impl_->current);
  PVBindings result;
  for (const auto& entry : snapshot->entries) result.emplace(entry.first, entry.second->binding);
  return result;
}

std::shared_ptr<PVRegistry::Prepared> PVRegistry::prepare(PVBindings desired, const Wrappers& wrappers) const {
  auto result = std::make_shared<Prepared>();
  result->previous = std::atomic_load(&impl_->current);
  auto next = std::make_shared<Snapshot>();
  auto names = std::make_shared<std::set<std::string>>();
  for (auto& entry : desired) {
    const auto& name = entry.first;
    if (name.empty() || name.find('\0') != std::string::npos || !entry.second.pv || !entry.second.owner)
      throw std::invalid_argument("invalid endpoint binding: " + name);
    const auto old = result->previous->entries.find(name);
    if (old != result->previous->entries.end() && old->second->binding.owner == entry.second.owner &&
        sameAccessAssignment(old->second->binding.access, entry.second.access)) {
      next->entries.emplace(name, old->second);
    } else {
      auto registration = std::make_shared<Registration>();
      registration->binding = std::move(entry.second);
      const auto wrapper = wrappers.find(name);
      if (wrapper != wrappers.end()) registration->wrapper = wrapper->second;
      next->entries.emplace(name, std::move(registration));
    }
    names->insert(name);
  }
  next->names = std::move(names);
  for (const auto& entry : result->previous->entries) {
    const auto keep = next->entries.find(entry.first);
    if (keep == next->entries.end() || keep->second != entry.second) result->retiring.push_back(entry.second);
  }
  result->next = std::move(next);
  return result;
}

bool PVRegistry::publish(const std::shared_ptr<Prepared>& prepared,
                          const std::function<bool(std::string&)>& beforeCommit, std::string& error) {
  {
    std::lock_guard<std::mutex> guard(impl_->publishMutex);
    if (!prepared || !prepared->next || prepared->previous != std::atomic_load(&impl_->current)) {
      error = "endpoint publication changed during preparation";
      return false;
    }
    if (beforeCommit && !beforeCommit(error)) return false;
    for (const auto& registration : prepared->retiring) registration->active = false;
    std::atomic_store(&impl_->current, prepared->next);
  }
  error.clear();
  return true;
}

void PVRegistry::finish(const std::shared_ptr<Prepared>& prepared) {
  for (const auto& registration : prepared->retiring) registration->close();
}

void PVRegistry::clear() {
  std::string error;
  const auto prepared = prepare({});
  if (!publish(prepared, {}, error)) throw std::logic_error(error);
  finish(prepared);
}
void PVRegistry::onSearch(Search& search) {
  const auto snapshot = std::atomic_load(&impl_->current);
  for (auto& name : search) if (snapshot->entries.count(name.name())) name.claim();
}
void PVRegistry::onCreate(Channel&& channel) {
  const auto snapshot = std::atomic_load(&impl_->current);
  const auto found = snapshot->entries.find(channel->name());
  if (found == snapshot->entries.end()) return;
  const auto registration = found->second;
  const std::shared_ptr<pvxs::server::ChannelControl> control(std::move(channel));
  Channel tracked = std::make_unique<TrackedChannel>(control, registration);
  if (registration->wrapper) tracked = registration->wrapper(std::move(tracked));
  if (!tracked) return;
  registration->binding.pv.attach(std::move(tracked));
  // Retirement may have drained the tracked channel list before attach
  // completed. Check after attaching; later retirements will find the channel
  // in that list. Operation callbacks also fence all retired registrations.
  if (!registration->active) control->close();
}
pvxs::server::Source::List PVRegistry::onList() {
  auto names = std::atomic_load(&impl_->current)->names;
  return {std::move(names), true};
}

} // namespace redis_pvxs_ioc
