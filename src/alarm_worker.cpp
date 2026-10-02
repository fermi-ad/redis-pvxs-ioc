#include "redis_pvxs_ioc/alarm_publisher.h"
#include "hiredis.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace redis_pvxs_ioc {
namespace {
using Clock = std::chrono::steady_clock;
struct Budget {
  const size_t limit;
  std::atomic<size_t> used{0}, peak{0};
  explicit Budget(size_t bytes) : limit(bytes) {}
  void acquire(size_t bytes) {
    auto current = used.load();
    do {
      if (bytes > limit || current > limit - bytes) throw std::runtime_error("alarm state budget exceeded during preparation");
    } while (!used.compare_exchange_weak(current, current + bytes));
    auto prior = peak.load();
    while (prior < current + bytes && !peak.compare_exchange_weak(prior, current + bytes)) {}
  }
};
struct Reservation {
  std::shared_ptr<Budget> budget;
  size_t bytes;
  Reservation(std::shared_ptr<Budget> value, size_t count) : budget(std::move(value)), bytes(count) { budget->acquire(bytes); }
  ~Reservation() { budget->used.fetch_sub(bytes); }
};
struct StoredState {
  int severity = 0, status = 0;
  size_t length = 0;
  std::array<char, 512> message{};
  static StoredState encode(const AlarmState& state, bool& truncated) noexcept {
    StoredState result;
    result.severity = state.severity; result.status = state.status;
    truncated = state.message.size() > result.message.size();
    result.length = std::min(state.message.size(), result.message.size() - (truncated ? 3u : 0u));
    if (truncated)
      while (result.length && (static_cast<unsigned char>(state.message[result.length]) & 0xc0u) == 0x80u) --result.length;
    std::memcpy(result.message.data(), state.message.data(), result.length);
    if (truncated) { std::memcpy(result.message.data() + result.length, "...", 3); result.length += 3; }
    return result;
  }
  bool operator==(const StoredState& other) const {
    return severity == other.severity && status == other.status && length == other.length &&
           std::memcmp(message.data(), other.message.data(), length) == 0;
  }
  AlarmState decode() const { return {severity, status, std::string(message.data(), length)}; }
};
enum class SendResult { Accepted, Rejected, Unavailable };
}

struct AlarmPublisher::Slot {
  std::shared_ptr<Reservation> reservation;
  std::string name;
  uint64_t id = 0, revision = 1, delivered = 0, deliveredEpoch = 0;
  bool active = false, retired = false, force = true;
  StoredState latest;
};

struct AlarmPublisher::Impl {
  struct Event {
    std::weak_ptr<Slot> slot;
    StoredState state;
    uint64_t revision = 0;
    std::time_t timestamp = 0;
  };
  static_assert(sizeof(Event) <= kAlarmQueueEntryBytes);
  const std::string host, stream, user, password;
  const int port;
  const AlarmPublisherOptions options;
  std::shared_ptr<Budget> budget;
  std::shared_ptr<Reservation> ringReservation;
  std::vector<Event> ring;
  size_t head = 0, count = 0;
  std::map<uint64_t, std::shared_ptr<Slot>> slots;
  std::map<std::string, std::map<uint64_t, std::weak_ptr<Slot>>> names;
  uint64_t nextId = 0, lastId = 0, epoch = 1;
  bool enabled = false, stopping = false, recovering = true, live = false;
  AlarmPublisherStatus counters;
  mutable std::mutex mutex;
  std::mutex stopMutex;
  std::condition_variable changed;
  std::thread worker;
  redisContext* context = nullptr; // worker only; freed after join
  bool authenticated = false;

  Impl(std::string h, int p, std::string s, std::string u, std::string pw, AlarmPublisherOptions settings)
      : host(std::move(h)), stream(std::move(s)), user(std::move(u)), password(std::move(pw)),
        port(p), options(settings), budget(std::make_shared<Budget>(settings.stateBytes)) {
    if (!options.queueEntries || !options.ioTimeoutMs || !options.retryMs || !options.heartbeatMs ||
        options.queueEntries > options.stateBytes / kAlarmQueueEntryBytes)
      throw std::invalid_argument("alarm queue must fit within a positive state budget");
    ringReservation = std::make_shared<Reservation>(budget, options.queueEntries * kAlarmQueueEntryBytes);
    ring.resize(options.queueEntries);
  }
  ~Impl() { if (context) redisFree(context); }
  bool current(const std::shared_ptr<Slot>& slot) const {
    if (!slot->active || slot->retired) return false;
    const auto found = names.find(slot->name);
    if (found == names.end()) return false;
    for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
      if (const auto candidate = it->second.lock(); candidate && candidate->active && !candidate->retired) return candidate == slot;
    return false;
  }
  bool pending(const Slot& slot) const { return slot.force || slot.deliveredEpoch != epoch || slot.delivered < slot.revision; }
  void discardQueue() {
    counters.discardedTransitions += count;
    while (count) { ring[head].slot.reset(); head = (head + 1) % ring.size(); --count; }
  }
  void remove(const std::shared_ptr<Slot>& slot) {
    std::lock_guard<std::mutex> guard(mutex);
    slot->active = false; slot->retired = true;
    slots.erase(slot->id);
    const auto name = names.find(slot->name);
    if (name != names.end()) {
      name->second.erase(slot->id);
      if (name->second.empty()) names.erase(name);
    }
    changed.notify_all();
  }
  void update(const std::shared_ptr<Slot>& slot, const AlarmState& state) noexcept {
    bool truncated = false;
    const auto encoded = StoredState::encode(state, truncated);
    std::lock_guard<std::mutex> guard(mutex);
    if (stopping || slot->retired || slot->latest == encoded) return;
    slot->latest = encoded; ++slot->revision;
    if (truncated) ++counters.messagesTruncated;
    if (enabled && current(slot)) {
      if (live && !recovering && count < ring.size()) {
        auto& entry = ring[(head + count++) % ring.size()];
        entry.slot = slot; entry.state = encoded; entry.revision = slot->revision; entry.timestamp = std::time(nullptr);
      } else {
        ++counters.coalescedUpdates;
        if (count == ring.size()) { discardQueue(); ++epoch; recovering = true; }
      }
    }
    changed.notify_all();
  }
  redisReply* command(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::vector<size_t> lengths;
    for (const auto& arg : args) { argv.push_back(arg.data()); lengths.push_back(arg.size()); }
    return static_cast<redisReply*>(redisCommandArgv(context, static_cast<int>(argv.size()), argv.data(), lengths.data()));
  }
  void reset() { if (context) redisFree(context); context = nullptr; authenticated = false; }
  SendResult connect() {
    const timeval timeout{static_cast<time_t>(options.ioTimeoutMs / 1000), static_cast<suseconds_t>((options.ioTimeoutMs % 1000) * 1000)};
    if (!context || context->err) {
      reset();
      context = redisConnectWithTimeout(host.c_str(), port, timeout);
      if (!context || context->err || redisSetTimeout(context, timeout) != REDIS_OK) { reset(); return SendResult::Unavailable; }
    }
    if (!authenticated && (!user.empty() || !password.empty())) {
      const auto reply = command(user.empty() ? std::vector<std::string>{"AUTH", password}
                                              : std::vector<std::string>{"AUTH", user, password});
      const auto result = !reply ? SendResult::Unavailable
          : reply->type == REDIS_REPLY_ERROR ? SendResult::Rejected : SendResult::Accepted;
      freeReplyObject(reply);
      if (result != SendResult::Accepted) { if (result == SendResult::Unavailable) reset(); return result; }
    }
    authenticated = true;
    return SendResult::Accepted;
  }
  SendResult send(const std::shared_ptr<Slot>& slot, const Event& event) {
    auto result = connect();
    if (result != SendResult::Accepted) return result;
    std::vector<std::string> args{"XADD", stream, "MAXLEN", "99999", "*"};
    for (const auto& field : makeAlarmStreamFields(slot->name, event.state.decode(), event.timestamp)) {
      args.push_back(field.first); args.push_back(field.second);
    }
    const auto reply = command(args);
    result = !reply ? SendResult::Unavailable
        : reply->type == REDIS_REPLY_STRING ? SendResult::Accepted : SendResult::Rejected;
    freeReplyObject(reply);
    if (result == SendResult::Unavailable) reset();
    return result;
  }
  SendResult ping() {
    auto result = connect();
    if (result != SendResult::Accepted) return result;
    const auto reply = command({"PING"});
    result = !reply ? SendResult::Unavailable
        : reply->type == REDIS_REPLY_STATUS ? SendResult::Accepted : SendResult::Rejected;
    freeReplyObject(reply);
    if (result == SendResult::Unavailable) reset();
    return result;
  }
  void run() {
    auto retryAt = Clock::time_point{}, heartbeat = Clock::now();
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping) {
      if (!enabled) { changed.wait(lock); continue; }
      if (Clock::now() < retryAt) { changed.wait_until(lock, retryAt, [&] { return stopping; }); continue; }
      Event event;
      std::shared_ptr<Slot> selected;
      bool reconciliation = false;
      while (count && !selected) {
        event = std::move(ring[head]); ring[head].slot.reset();
        head = (head + 1) % ring.size(); --count;
        selected = event.slot.lock();
        if (selected && !current(selected)) selected.reset();
        if (!selected) ++counters.discardedTransitions;
      }
      if (!selected && !slots.empty()) {
        auto it = slots.upper_bound(lastId);
        for (size_t checked = 0; checked < slots.size(); ++checked) {
          if (it == slots.end()) it = slots.begin();
          const auto candidate = it->second; ++it;
          if (current(candidate) && pending(*candidate)) {
            selected = candidate; event.slot = selected; event.state = selected->latest;
            event.revision = selected->revision; event.timestamp = std::time(nullptr);
            reconciliation = true; lastId = selected->id; break;
          }
        }
      }
      if (!selected && live) recovering = false;
      if (!selected) {
        bool hasActive = false;
        for (const auto& entry : slots) if (current(entry.second)) { hasActive = true; break; }
        if (!hasActive || Clock::now() < heartbeat) {
          if (hasActive) changed.wait_until(lock, heartbeat);
          else changed.wait(lock);
          continue;
        }
      }
      const auto sendingEpoch = epoch;
      lock.unlock();
      SendResult result;
      try { result = selected ? send(selected, event) : ping(); }
      catch (...) { reset(); result = SendResult::Unavailable; }
      lock.lock();
      heartbeat = Clock::now() + std::chrono::milliseconds(options.heartbeatMs);
      if (result == SendResult::Accepted) {
        live = true; counters.lastError.clear();
        if (selected) {
          ++counters.sent;
          if (reconciliation) ++counters.reconciled;
          if (current(selected)) {
            selected->delivered = std::max(selected->delivered, event.revision);
            selected->deliveredEpoch = sendingEpoch; selected->force = false;
          }
        }
      } else {
        if (result == SendResult::Rejected) { ++counters.rejected; counters.lastError = "Redis rejected alarm delivery"; }
        else { ++counters.transportFailures; counters.lastError = "Redis alarm transport unavailable"; live = false; }
        discardQueue(); ++epoch; recovering = true;
        retryAt = Clock::now() + std::chrono::milliseconds(options.retryMs);
      }
    }
  }
  void stop() {
    std::lock_guard<std::mutex> serialized(stopMutex);
    { std::lock_guard<std::mutex> guard(mutex); stopping = true; changed.notify_all(); }
    if (worker.joinable()) worker.join();
  }
};

AlarmPublisher::Registration::Registration(std::weak_ptr<Impl> owner, std::shared_ptr<Slot> slot)
    : owner_(std::move(owner)), slot_(std::move(slot)) {}
AlarmPublisher::Registration::~Registration() { if (const auto owner = owner_.lock()) owner->remove(slot_); }
void AlarmPublisher::Registration::update(const AlarmState& state) noexcept { if (const auto owner = owner_.lock()) owner->update(slot_, state); }
void AlarmPublisher::Registration::activate() noexcept {
  if (const auto owner = owner_.lock()) {
    std::lock_guard<std::mutex> guard(owner->mutex);
    if (!slot_->retired && !slot_->active) { slot_->active = true; slot_->force = true; owner->changed.notify_all(); }
  }
}
void AlarmPublisher::Registration::retire() noexcept {
  if (const auto owner = owner_.lock()) {
    std::lock_guard<std::mutex> guard(owner->mutex);
    slot_->active = false; slot_->retired = true; owner->changed.notify_all();
  }
}

AlarmPublisher::AlarmPublisher(std::string host, int port, std::string stream, std::string user,
                               std::string password, AlarmPublisherOptions options)
    : impl_(std::make_shared<Impl>(std::move(host), port, std::move(stream), std::move(user), std::move(password), options)) {
  impl_->worker = std::thread([impl = impl_] { impl->run(); });
}
AlarmPublisher::~AlarmPublisher() { stop(); }
void AlarmPublisher::stop() { impl_->stop(); }
void AlarmPublisher::activate() noexcept {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->enabled = true; impl_->changed.notify_all();
}
std::shared_ptr<AlarmPublisher::Registration> AlarmPublisher::prepare(const std::string& name, const AlarmState& state) {
  if (name.size() > (std::numeric_limits<size_t>::max() - 1024) / 2) throw std::runtime_error("alarm name too large");
  auto reservation = std::make_shared<Reservation>(impl_->budget, 1024 + 2 * name.size());
  auto slot = std::make_shared<Slot>();
  slot->reservation = std::move(reservation); slot->name = name;
  bool truncated = false;
  slot->latest = StoredState::encode(state, truncated);
  auto registration = std::shared_ptr<Registration>(new Registration(impl_, slot));
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->stopping) throw std::runtime_error("alarm publisher stopped");
    slot->id = ++impl_->nextId;
    impl_->slots.emplace(slot->id, slot);
    impl_->names[name].emplace(slot->id, slot);
    if (truncated) ++impl_->counters.messagesTruncated;
  }
  return registration;
}
bool AlarmPublisher::connected() const { std::lock_guard<std::mutex> guard(impl_->mutex); return impl_->live; }
const std::string& AlarmPublisher::stream() const { return impl_->stream; }
AlarmPublisherStatus AlarmPublisher::status() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  auto result = impl_->counters;
  result.registrations = impl_->slots.size(); result.queued = impl_->count;
  result.reservedBytes = impl_->budget->used.load(); result.peakBytes = impl_->budget->peak.load();
  result.byteLimit = impl_->options.stateBytes; result.queueLimit = impl_->ring.size();
  for (const auto& entry : impl_->slots) if (impl_->current(entry.second)) {
    ++result.active;
    if (impl_->pending(*entry.second)) ++result.pending;
  }
  result.state = impl_->stopping ? "stopped" : !impl_->enabled ? "staged" : !result.active ? "idle"
      : !impl_->live ? "disconnected" : (impl_->recovering || result.pending || result.queued) ? "recovering" : "ready";
  return result;
}

} // namespace redis_pvxs_ioc
