#include "redis_pvxs_ioc/runtime.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <alarm.h>

#include <pvxs/server.h>

#include "RedisAdapter.hpp"

#include "redis_pvxs_ioc/alarm_publisher.h"
#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/queued_exec.h"
#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {
namespace {

bool sameRouteTarget(const RouteConfig& lhs, const RouteConfig& rhs) {
  return lhs.backend == rhs.backend && lhs.key == rhs.key;
}

bool sameRouteTarget(const RouteConfig& lhs, const ConfirmConfig& rhs) {
  return lhs.backend == rhs.backend && lhs.key == rhs.key;
}

std::shared_ptr<RedisAdapter> resolveBackend(const RedisBackendRegistry& redisBackends,
                                             const std::string& alias,
                                             const std::string& fullName,
                                             const std::string& routeName) {
  const auto it = redisBackends.find(alias);
  if (it == redisBackends.end()) {
    throw std::runtime_error("unknown redis backend '" + alias + "' for " + fullName + " " + routeName);
  }
  return it->second;
}

template <typename T, bool Array>
class TypedRuntime final : public PVRuntimeBase, public std::enable_shared_from_this<TypedRuntime<T, Array>> {
public:
  using ValueType = std::conditional_t<Array, std::vector<T>, T>;

  TypedRuntime(const ServerConfig& serverConfig, PVConfig config,
               std::shared_ptr<RedisAdapter> readRedis,
               std::shared_ptr<RedisAdapter> writeRedis,
               std::shared_ptr<RedisAdapter> confirmRedis,
               std::shared_ptr<AlarmPublisher> alarmPublisher, uint64_t generation,
               std::shared_ptr<OperationQueue> operations, OperationLimitsConfig limits)
      : config_(std::move(config)), readRedis_(std::move(readRedis)),
        writeRedis_(std::move(writeRedis)), confirmRedis_(std::move(confirmRedis)),
        alarmPublisher_(std::move(alarmPublisher)), fullName_(fullPVName(serverConfig, config_)),
        generation_(generation), operations_(std::move(operations)), limits_(limits),
        confirmationMs_(config_.confirm ? config_.confirm->timeoutMs : 0),
        pv_(config_.write ? pvxs::server::SharedPV::buildMailbox() : pvxs::server::SharedPV::buildReadonly()) {}

  ~TypedRuntime() override { deactivate("runtime destroyed"); }

  void initialize() {
    if constexpr (Array) lastRaw_ = initialVectorOr<T>(config_.initialValue);
    else lastRaw_ = initialScalarOr<T>(config_.initialValue, T{});
    const auto snapshot = readRedis_->getStreamSnapshot(config_.read.key);
    readCursor_ = snapshot.id;
    ValueType decoded{};
    if (snapshot.present() && decode(snapshot.fields, decoded) && RA_Time(snapshot.id).ok()) {
      lastRaw_ = std::move(decoded);
      lastTimestampNs_ = static_cast<uint64_t>(RA_Time(snapshot.id).value);
      sourceValid_ = true;
    } else {
      sourceError_ = snapshot.present() ? "Invalid Redis snapshot" : "No source data; using initial fallback";
    }
    auto initial = createInitialValue(config_);
    populateValue(initial);
    pv_.open(initial);
    auto weak = this->weak_from_this();
    if (config_.write) {
      pv_.onPut([weak](pvxs::server::SharedPV&, std::unique_ptr<pvxs::server::ExecOp>&& op,
                       pvxs::Value&& value) {
        if (const auto self = weak.lock()) self->handlePut(std::move(op), std::move(value));
        else op->error("runtime unavailable");
      });
    }
    const bool readConfirms = config_.confirm && sameRouteTarget(config_.read, *config_.confirm);
    readReader_ = readRedis_->subscribeStream(config_.read.key,
        [weak, readConfirms](const auto&, const auto&, const RedisAdapter::StreamBatch& data) {
          if (const auto self = weak.lock()) self->handleRead(data, true, readConfirms);
        }, readCursor_);
    if (config_.confirm && !readConfirms) {
      const auto confirmation = confirmRedis_->getStreamSnapshot(config_.confirm->key);
      confirmReader_ = confirmRedis_->subscribeStream(config_.confirm->key,
          [weak](const auto&, const auto&, const RedisAdapter::StreamBatch& data) {
            if (const auto self = weak.lock()) self->handleRead(data, false, true);
          }, confirmation.id);
    }
  }

  const PVConfig& config() const override { return config_; }
  const std::string& fullName() const override { return fullName_; }
  pvxs::server::SharedPV& sharedPV() override { return pv_; }
  bool structurallyCompatible(const PVConfig& config) const override { return sameReaderTopology(config_, config); }
  void activate() override { committed_ = true; }

  void setAlarmPublisher(std::shared_ptr<AlarmPublisher> publisher) override {
    std::lock_guard<std::mutex> lock(mutex_);
    alarmPublisher_ = std::move(publisher);
  }

  void reconfigure(const PVConfig& config, uint64_t generation) override {
    AlarmState state;
    bool transition = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_) return;
      const bool sameTransform = config_.transform.has_value() == config.transform.has_value() &&
          (!config_.transform || (config_.transform->scale == config.transform->scale &&
                                  config_.transform->offset == config.transform->offset));
      if (!sameTransform) {
        ++commandEpoch_;
        failPendingLocked("write transform changed during reload");
      }
      config_ = config;
      generation_ = generation;
      auto value = pv_.fetch();
      state = populateValue(value);
      transition = recordAlarmLocked(state);
      pv_.post(value);
    }
    publishAlarm(state, transition);
  }

  void deactivate(const std::string& reason) override {
    if (!alive_.exchange(false)) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++commandEpoch_;
      failPendingLocked(reason);
    }
    readReader_.reset();
    confirmReader_.reset();
    if (pv_.isOpen()) pv_.close();
  }

private:
  struct PendingPut {
    uint64_t id = 0;
    ValueType expectedRaw{};
    std::string afterId;
    bool done = false;
    std::string error;
    std::condition_variable cv;
  };

  bool decode(const RedisAdapter::Attrs& fields, ValueType& raw) const {
    if constexpr (Array) return RedisAdapter::decodeArray(fields, raw, limits_.maxPayloadBytes);
    else return RedisAdapter::decodeScalar(fields, raw, limits_.maxPayloadBytes);
  }

  AlarmState populateValue(pvxs::Value& value) {
    const auto present = applyForwardTransform(config_, lastRaw_);
    if constexpr (Array) assignArrayValue(value, present);
    else assignScalarValue(value, present);
    applyStandardMetadata(value, config_);
    if (lastTimestampNs_) applyTimestamp(value, lastTimestampNs_);
    else {
      value["timeStamp.secondsPastEpoch"] = static_cast<int64_t>(0);
      value["timeStamp.nanoseconds"] = static_cast<int32_t>(0);
      value["timeStamp.userTag"] = static_cast<int32_t>(0);
    }
    AlarmState state{epicsSevNone, epicsAlarmNone, ""};
    if (!sourceValid_) state = {epicsSevInvalid, epicsAlarmUDF, sourceError_};
    else if constexpr (!Array && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
      const int prior = value["alarm.status"].as<int>();
      state = evaluateNumericAlarm(config_, static_cast<double>(present), prior);
    }
    applyAlarmFields(value, config_, state);
    return state;
  }

  bool recordAlarmLocked(const AlarmState& state) {
    const bool changed = state.status != lastPublishedStatus_ || state.severity != lastPublishedSeverity_;
    lastPublishedStatus_ = state.status;
    lastPublishedSeverity_ = state.severity;
    return changed;
  }

  void publishAlarm(const AlarmState& state, bool changed) {
    std::shared_ptr<AlarmPublisher> publisher;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (changed && alive_.load() && committed_.load()) publisher = alarmPublisher_;
    }
    if (publisher) publisher->publishTransition(fullName_, state);
  }

  void handleRead(const RedisAdapter::StreamBatch& data, bool updateValue, bool canConfirm) {
    // Evaluate every observed sample for alarms and confirmations, including
    // intermediate values in a batch. Confirmation data never changes readback.
    for (const auto& entry : data) {
      if (!alive_) return;
      ValueType raw{};
      const auto timestamp = RA_Time(entry.first);
      if (!decode(entry.second, raw) || !timestamp.ok()) {
        if (updateValue) markInvalid("Invalid Redis payload or source timestamp");
        continue;
      }
      handleRawUpdate(raw, static_cast<uint64_t>(timestamp.value), updateValue, canConfirm, entry.first);
    }
  }

  void markInvalid(const std::string& error) {
    AlarmState state;
    bool transition = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_) return;
      sourceValid_ = false;
      sourceError_ = error;
      auto value = pv_.fetch();
      state = populateValue(value);
      transition = recordAlarmLocked(state);
      pv_.post(value);
    }
    publishAlarm(state, transition);
  }

  void handleRawUpdate(const ValueType& raw, uint64_t timestamp, bool updateValue,
                       bool canConfirm = false, const std::string& streamId = {}) {
    AlarmState state;
    bool transition = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_) return;
      if (canConfirm) {
        for (auto& item : pendingPuts_) {
          auto& pending = *item.second;
          if (!pending.done && RedisAdapter::compareStreamIds(streamId, pending.afterId) > 0 &&
              valuesEqual(pending.expectedRaw, raw)) {
            pending.done = true;
            pending.cv.notify_all();
          }
        }
      }
      if (!updateValue) return;
      lastRaw_ = raw;
      lastTimestampNs_ = timestamp;
      sourceValid_ = true;
      sourceError_.clear();
      auto value = pv_.fetch();
      state = populateValue(value);
      transition = recordAlarmLocked(state);
      // Serialize posts with config changes and deactivation, not just fetches.
      pv_.post(value);
    }
    publishAlarm(state, transition);
  }

  void failPendingLocked(const std::string& reason) {
    for (auto& item : pendingPuts_) {
      item.second->done = true;
      item.second->error = reason;
      item.second->cv.notify_all();
    }
  }

  void handlePut(std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
    // Admission must not wait behind the worker's network dispatch fence.
    if (!alive_ || !committed_) { op->error("generation is no longer active"); return; }
    const auto epoch = commandEpoch_.load();
    size_t bytes = sizeof(T);
    try {
      if constexpr (Array) {
        const auto array = value["value"].as<pvxs::shared_array<const T>>();
        if (array.size() > limits_.maxPayloadBytes / sizeof(T)) {
          op->error("write payload exceeds max_payload_bytes"); return;
        }
        bytes = array.size() * sizeof(T);
      } else if constexpr (std::is_same_v<T, std::string>) {
        bytes = value["value"].as<std::string>().size();
      }
      if (bytes > limits_.maxPayloadBytes) { op->error("write payload exceeds max_payload_bytes"); return; }
    } catch (const std::exception& ex) {
      op->error(std::string("invalid put value: ") + ex.what()); return;
    }
    const auto deadline = OperationQueue::Clock::now() + std::chrono::milliseconds(
        limits_.operationTimeoutMs.value_or(static_cast<uint32_t>(std::max(5000, confirmationMs_ + 2000))));
    auto request = QueuedExec::create(std::move(op), operations_);
    auto self = this->shared_from_this();
    const auto ticket = operations_->submit({fullName_, bytes, deadline,
        [self, request, epoch, deadline, value = std::move(value)]() mutable {
          self->dispatchPut(request, std::move(value), epoch, deadline);
        },
        [request](OperationFailure, const std::string& message) { request->error(message); }});
    request->ticket(operations_, ticket);
  }

  void dispatchPut(const std::shared_ptr<QueuedExec>& request, pvxs::Value value,
                   uint64_t epoch, OperationQueue::Clock::time_point deadline) {
    if (request->stopped()) return;
    PVConfig current;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_ || !committed_ || commandEpoch_ != epoch) {
        request->error("write configuration changed before dispatch"); return;
      }
      current = config_;
    }
    ValueType present{};
    try {
      if constexpr (Array) present = arrayValueFrom<T>(value);
      else present = scalarValueFrom<T>(value);
    } catch (const std::exception& ex) {
      request->error(std::string("invalid put value: ") + ex.what()); return;
    }
    const auto raw = applyInverseTransform(current, present);
    std::shared_ptr<PendingPut> pending;
    if (current.confirm) {
      if (request->stopped()) return;
      const auto snapshot = confirmRedis_->getStreamSnapshot(current.confirm->key);
      if (!snapshot.connected) { request->error("confirmation backend unavailable"); return; }
      pending = std::make_shared<PendingPut>();
      pending->expectedRaw = raw;
      pending->afterId = snapshot.id;
    }
    RA_Time writeTime;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_ || commandEpoch_ != epoch) { request->error("write configuration changed"); return; }
      if (request->stopped()) return;
      if (OperationQueue::Clock::now() >= deadline) {
        request->error("total operation deadline exceeded before dispatch"); return;
      }
      const auto operation = request->operation();
      if (!operation) return;
      if (!authorizeWriteDispatch(*operation, value)) {
        request->error("access denied at dispatch"); return;
      }
      if (request->stopped()) return;
      if (pending) {
        pending->id = ++nextPendingId_;
        pendingPuts_[pending->id] = pending;
      }
      // Only one worker per canonical PV reaches this acceptance boundary.
      // Retirement is serialized with dispatch; no command is ever replayed.
      try { writeTime = writeToRedis(current, raw); }
      catch (...) { if (pending) pendingPuts_.erase(pending->id); throw; }
      if (!writeTime.ok() && pending) pendingPuts_.erase(pending->id);
    }
    if (!writeTime.ok()) { request->error("redis write failed; not retried"); return; }
    if (!pending) {
      if (request->stopped()) return;
      if (sameRouteTarget(current.read, *current.write))
        handleRawUpdate(raw, static_cast<uint64_t>(writeTime.value), true);
      if (OperationQueue::Clock::now() >= deadline)
        request->error("total operation deadline exceeded after dispatch; not retried");
      else request->reply();
      return;
    }
    // Register outside mutex_: wake() also calls back if already stopped.
    // Taking the waiter's mutex closes the predicate-check/wait wake-up gap.
    request->wake([owner = this->weak_from_this(), weak = std::weak_ptr<PendingPut>(pending)] {
      if (const auto self = owner.lock()) {
        std::lock_guard<std::mutex> guard(self->mutex_);
        if (const auto item = weak.lock()) item->cv.notify_all();
      }
    });
    std::unique_lock<std::mutex> lock(mutex_);
    const auto confirmationDeadline = std::min(deadline,
        OperationQueue::Clock::now() + std::chrono::milliseconds(current.confirm->timeoutMs));
    const bool completed = pending->cv.wait_until(lock, confirmationDeadline, [&] {
      return pending->done || request->stopped() || !alive_ || commandEpoch_ != epoch;
    });
    pendingPuts_.erase(pending->id);
    const auto error = pending->error;
    const bool confirmed = completed && pending->done;
    lock.unlock();
    if (request->stopped()) return;
    if (!error.empty()) request->error(error);
    else if (OperationQueue::Clock::now() >= deadline)
      request->error("total operation deadline exceeded after dispatch; not retried");
    else if (!confirmed) request->error("backend confirmation timeout; not retried");
    else request->reply();
  }

  RA_Time writeToRedis(const PVConfig& config, const ValueType& raw) {
    if constexpr (Array) return writeRedis_->addSingleList<T>(config.write->key, raw);
    else if constexpr (std::is_same_v<T, double>) return writeRedis_->addSingleDouble(config.write->key, raw);
    else return writeRedis_->addSingleValue<T>(config.write->key, raw);
  }

  mutable std::mutex mutex_;
  PVConfig config_;
  std::shared_ptr<RedisAdapter> readRedis_, writeRedis_, confirmRedis_;
  std::shared_ptr<AlarmPublisher> alarmPublisher_;
  std::string fullName_;
  uint64_t generation_ = 0;
  std::atomic<uint64_t> commandEpoch_{0};
  std::shared_ptr<OperationQueue> operations_;
  const OperationLimitsConfig limits_;
  const int confirmationMs_;
  pvxs::server::SharedPV pv_;
  std::atomic<bool> alive_{true};       // false once retired
  std::atomic<bool> committed_{false};  // true after staging commits
  RedisAdapter::ReaderHandle readReader_, confirmReader_;
  std::string readCursor_;
  ValueType lastRaw_{};
  uint64_t lastTimestampNs_ = 0;
  bool sourceValid_ = false;
  std::string sourceError_;
  int lastPublishedStatus_ = epicsAlarmNone;
  int lastPublishedSeverity_ = epicsSevNone;
  uint64_t nextPendingId_ = 0;
  std::unordered_map<uint64_t, std::shared_ptr<PendingPut>> pendingPuts_;
};

template <typename T, bool Array>
std::shared_ptr<PVRuntimeBase> makeTypedRuntime(const ServerConfig& serverConfig,
                                                const PVConfig& config,
                                                const RedisBackendRegistry& redisBackends,
                                                const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                                const uint64_t generation,
                                                std::shared_ptr<OperationQueue> operations,
                                                OperationLimitsConfig limits) {
  const auto fullName = fullPVName(serverConfig, config);
  auto runtime = std::make_shared<TypedRuntime<T, Array>>(
      serverConfig,
      config,
      resolveBackend(redisBackends, config.read.backend, fullName, "read route"),
      config.write ? resolveBackend(redisBackends, config.write->backend, fullName, "write route") : nullptr,
      config.confirm ? resolveBackend(redisBackends, config.confirm->backend, fullName, "confirm route") : nullptr,
      alarmPublisher,
      generation, std::move(operations), limits);
  runtime->initialize();
  return runtime;
}

}  // namespace

std::shared_ptr<PVRuntimeBase> makeRuntime(const ServerConfig& serverConfig,
                                           const PVConfig& config,
                                           const RedisBackendRegistry& redisBackends,
                                           const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                           const uint64_t generation,
                                                std::shared_ptr<OperationQueue> operations,
                                                OperationLimitsConfig limits) {
  if (!operations) {
    static auto defaultOperations = std::make_shared<OperationQueue>();
    operations = defaultOperations;
  }
  switch (config.shape) {
  case Shape::Scalar:
    switch (config.type) {
    case PrimitiveType::Boolean: return makeTypedRuntime<bool, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Float32: return makeTypedRuntime<float, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Float64: return makeTypedRuntime<double, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::String: return makeTypedRuntime<std::string, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    }
    break;
  case Shape::Array:
    switch (config.type) {
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Float32: return makeTypedRuntime<float, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Float64: return makeTypedRuntime<double, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits);
    case PrimitiveType::Boolean:
    case PrimitiveType::String:
      break;
    }
    break;
  }

  throw std::runtime_error("unsupported runtime type");
}

}  // namespace redis_pvxs_ioc
