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
#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {
namespace {

template <typename T, bool Array>
class TypedRuntime final : public PVRuntimeBase, public std::enable_shared_from_this<TypedRuntime<T, Array>> {
public:
  using ValueType = std::conditional_t<Array, std::vector<T>, T>;
  using ReaderValueType = ValueType;
  using ReaderList = typename RedisAdapter::TimeValList<ReaderValueType>;

  TypedRuntime(const ServerConfig& serverConfig,
               PVConfig config,
               std::shared_ptr<RedisAdapter> redis,
               std::shared_ptr<AlarmPublisher> alarmPublisher,
               const uint64_t generation)
      : config_(std::move(config)),
        redis_(std::move(redis)),
        alarmPublisher_(std::move(alarmPublisher)),
        fullName_(fullPVName(serverConfig, config_)),
        generation_(generation),
        pv_(config_.write ? pvxs::server::SharedPV::buildMailbox() : pvxs::server::SharedPV::buildReadonly()) {}

  ~TypedRuntime() override {
    deactivate("runtime destroyed");
  }

  void initialize() {
    auto initial = createInitialValue(config_);
    loadInitialValue(initial);

    if (config_.write) {
      auto weak = this->weak_from_this();
      pv_.onPut([weak](pvxs::server::SharedPV& pv,
                       std::unique_ptr<pvxs::server::ExecOp>&& op,
                       pvxs::Value&& value) {
        if (const auto self = weak.lock()) {
          self->handlePut(pv, std::move(op), std::move(value));
        } else {
          op->error("runtime unavailable");
        }
      });
    }

    pv_.open(initial);
    registerReaders();
  }

  const PVConfig& config() const override {
    return config_;
  }

  const std::string& fullName() const override {
    return fullName_;
  }

  pvxs::server::SharedPV& sharedPV() override {
    return pv_;
  }

  bool structurallyCompatible(const PVConfig& config) const override {
    return sameReaderTopology(config_, config);
  }

  void reconfigure(const PVConfig& config, const uint64_t generation) override {
    ValueType lastRaw{};
    uint64_t lastTimestamp = 0;
    bool haveLastRaw = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      config_ = config;
      generation_ = generation;
      if constexpr (Array) {
        lastRaw = lastRaw_;
      } else {
        lastRaw = lastRaw_;
      }
      lastTimestamp = lastTimestampNs_;
      haveLastRaw = haveLastRaw_;
    }

    if (haveLastRaw) {
      handleRawUpdate(lastRaw, lastTimestamp, true);
    } else {
      auto value = pv_.fetch();
      applyStandardMetadata(value, config);
      applyTimestamp(value, lastTimestamp);
      if constexpr (!Array && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        applyAlarmFields(value, config, AlarmState{epicsSevNone, epicsAlarmNone, ""});
      }
      pv_.post(value);
    }
  }

  void deactivate(const std::string& reason) override {
    const bool wasActive = active_.exchange(false);
    if (!wasActive) {
      return;
    }

    std::vector<std::shared_ptr<PendingPut>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& item : pendingPuts_) {
        item.second->done = true;
        item.second->error = reason;
        pending.push_back(item.second);
      }
      pendingPuts_.clear();
    }
    for (const auto& item : pending) {
      item->cv.notify_all();
    }

    if (redis_) {
      redis_->removeReader(config_.read.key);
      if (config_.confirm && config_.confirm->key != config_.read.key) {
        redis_->removeReader(config_.confirm->key);
      }
    }

    if (pv_.isOpen()) {
      pv_.close();
    }
  }

private:
  struct PendingPut {
    uint64_t id = 0;
    ValueType expectedRaw{};
    bool done = false;
    std::string error;
    std::condition_variable cv;
  };

  void loadInitialValue(pvxs::Value& initial) {
    ValueType raw{};
    if constexpr (Array) {
      raw = initialVectorOr<T>(config_.initialValue);
    } else {
      raw = initialScalarOr<T>(config_.initialValue, T{});
    }

    uint64_t timestamp = 0;
    loadSnapshot(raw, timestamp);
    haveLastRaw_ = true;
    lastRaw_ = raw;
    lastTimestampNs_ = timestamp;

    const auto present = applyForwardTransform(config_, raw);
    if constexpr (Array) {
      assignArrayValue(initial, present);
    } else {
      assignScalarValue(initial, present);
    }

    applyTimestamp(initial, timestamp);

    if constexpr (!Array && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
      const auto alarmState = evaluateNumericAlarm(config_, static_cast<double>(present), epicsAlarmNone);
      applyAlarmFields(initial, config_, alarmState);
      lastPublishedStatus_ = alarmState.status;
      lastPublishedSeverity_ = alarmState.severity;
    } else {
      applyAlarmFields(initial, config_, AlarmState{epicsSevNone, epicsAlarmNone, ""});
    }
  }

  void loadSnapshot(ValueType& raw, uint64_t& timestamp) {
    if constexpr (Array) {
      const auto result = redis_->getSingleList<T>(config_.read.key, raw);
      timestamp = result.ok() ? static_cast<uint64_t>(result.value) : 0;
    } else if constexpr (std::is_same_v<T, double>) {
      const auto result = redis_->getSingleValue<double>(config_.read.key, raw);
      timestamp = result.ok() ? static_cast<uint64_t>(result.value) : 0;
    } else {
      const auto result = redis_->getSingleValue<T>(config_.read.key, raw);
      timestamp = result.ok() ? static_cast<uint64_t>(result.value) : 0;
    }
  }

  void registerReaders() {
    auto weak = this->weak_from_this();

    if constexpr (Array) {
      redis_->addListsReader<T>(config_.read.key, [weak](const std::string&, const std::string&, const ReaderList& data) {
        if (const auto self = weak.lock()) {
          self->handleRead(data, true);
        }
      });
    } else {
      redis_->addValuesReader<T>(config_.read.key, [weak](const std::string&, const std::string&, const ReaderList& data) {
        if (const auto self = weak.lock()) {
          self->handleRead(data, true);
        }
      });
    }

    if (config_.confirm && config_.confirm->key != config_.read.key) {
      if constexpr (Array) {
        redis_->addListsReader<T>(config_.confirm->key, [weak](const std::string&, const std::string&, const ReaderList& data) {
          if (const auto self = weak.lock()) {
            self->handleRead(data, false);
          }
        });
      } else {
        redis_->addValuesReader<T>(config_.confirm->key, [weak](const std::string&, const std::string&, const ReaderList& data) {
          if (const auto self = weak.lock()) {
            self->handleRead(data, false);
          }
        });
      }
    }
  }

  void handleRead(const ReaderList& data, const bool updateValue) {
    if (!active_.load() || data.empty()) {
      return;
    }
    const auto& latest = data.back();
    handleRawUpdate(latest.second, static_cast<uint64_t>(latest.first.value), updateValue);
  }

  void handleRawUpdate(const ValueType& raw, const uint64_t timestamp, const bool updateValue) {
    pvxs::Value value;
    AlarmState alarmState{epicsSevNone, epicsAlarmNone, ""};
    bool publishTransition = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_.load()) {
        return;
      }

      haveLastRaw_ = true;
      lastRaw_ = raw;
      lastTimestampNs_ = timestamp;
      completePendingLocked(raw);

      if (!updateValue) {
        return;
      }

      value = pv_.fetch();
      const auto currentConfig = config_;
      const auto present = applyForwardTransform(currentConfig, raw);

      if constexpr (Array) {
        assignArrayValue(value, present);
      } else {
        assignScalarValue(value, present);
      }

      applyStandardMetadata(value, currentConfig);
      applyTimestamp(value, timestamp);

      if constexpr (!Array && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
        const int priorStatus = value["alarm.status"].valid() ? value["alarm.status"].as<int>() : epicsAlarmNone;
        alarmState = evaluateNumericAlarm(currentConfig, static_cast<double>(present), priorStatus);
        applyAlarmFields(value, currentConfig, alarmState);
        publishTransition = (alarmState.status != lastPublishedStatus_ ||
                             alarmState.severity != lastPublishedSeverity_);
        lastPublishedStatus_ = alarmState.status;
        lastPublishedSeverity_ = alarmState.severity;
      } else {
        applyAlarmFields(value, currentConfig, AlarmState{epicsSevNone, epicsAlarmNone, ""});
      }
    }

    pv_.post(value);
    if (publishTransition && alarmPublisher_) {
      alarmPublisher_->publishTransition(fullName_, alarmState);
    }
  }

  void completePendingLocked(const ValueType& raw) {
    for (auto& item : pendingPuts_) {
      if (!item.second->done && valuesEqual(item.second->expectedRaw, raw)) {
        item.second->done = true;
        item.second->cv.notify_all();
      }
    }
  }

  void handlePut(pvxs::server::SharedPV&,
                 std::unique_ptr<pvxs::server::ExecOp>&& op,
                 pvxs::Value&& value) {
    if (!active_.load()) {
      op->error("generation is no longer active");
      return;
    }

    PVConfig currentConfig;
    uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      currentConfig = config_;
      generation = generation_;
    }

    ValueType present{};
    if constexpr (Array) {
      present = arrayValueFrom<T>(value);
    } else {
      present = scalarValueFrom<T>(value);
    }

    const auto raw = applyInverseTransform(currentConfig, present);

    std::shared_ptr<PendingPut> pending;
    if (currentConfig.confirm) {
      pending = std::make_shared<PendingPut>();
      pending->expectedRaw = raw;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending->id = ++nextPendingId_;
        pendingPuts_[pending->id] = pending;
      }
    }

    const auto writeTime = writeToRedis(currentConfig, raw);
    if (!writeTime.ok()) {
      if (pending) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingPuts_.erase(pending->id);
      }
      op->error("redis write failed");
      return;
    }

    if (!currentConfig.confirm) {
      if (currentConfig.write && currentConfig.write->key == currentConfig.read.key) {
        handleRawUpdate(raw, static_cast<uint64_t>(writeTime.value), true);
      }
      op->reply();
      return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    const auto timeout = std::chrono::milliseconds(currentConfig.confirm->timeoutMs);
    const bool completed = pending->cv.wait_for(lock, timeout, [&]() {
      return pending->done || !active_.load() || generation_ != generation;
    });

    pendingPuts_.erase(pending->id);

    if (!completed || !pending->done) {
      op->error("backend confirmation timeout");
      return;
    }
    if (!pending->error.empty()) {
      op->error(pending->error);
      return;
    }
    op->reply();
  }

  RA_Time writeToRedis(const PVConfig& config, const ValueType& raw) {
    if constexpr (Array) {
      return redis_->addSingleList<T>(config.write->key, raw);
    } else if constexpr (std::is_same_v<T, double>) {
      return redis_->addSingleDouble(config.write->key, raw);
    } else {
      return redis_->addSingleValue<T>(config.write->key, raw);
    }
  }

  mutable std::mutex mutex_;
  PVConfig config_;
  std::shared_ptr<RedisAdapter> redis_;
  std::shared_ptr<AlarmPublisher> alarmPublisher_;
  std::string fullName_;
  uint64_t generation_ = 0;
  pvxs::server::SharedPV pv_;
  std::atomic<bool> active_{true};
  ValueType lastRaw_{};
  bool haveLastRaw_ = false;
  uint64_t lastTimestampNs_ = 0;
  int lastPublishedStatus_ = epicsAlarmNone;
  int lastPublishedSeverity_ = epicsSevNone;
  uint64_t nextPendingId_ = 0;
  std::unordered_map<uint64_t, std::shared_ptr<PendingPut>> pendingPuts_;
};

template <typename T, bool Array>
std::shared_ptr<PVRuntimeBase> makeTypedRuntime(const ServerConfig& serverConfig,
                                                const PVConfig& config,
                                                const std::shared_ptr<RedisAdapter>& redis,
                                                const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                                const uint64_t generation) {
  auto runtime = std::make_shared<TypedRuntime<T, Array>>(serverConfig, config, redis, alarmPublisher, generation);
  runtime->initialize();
  return runtime;
}

}  // namespace

std::shared_ptr<PVRuntimeBase> makeRuntime(const ServerConfig& serverConfig,
                                           const PVConfig& config,
                                           const std::shared_ptr<RedisAdapter>& redis,
                                           const std::shared_ptr<AlarmPublisher>& alarmPublisher,
                                           const uint64_t generation) {
  switch (config.shape) {
  case Shape::Scalar:
    switch (config.type) {
    case PrimitiveType::Boolean: return makeTypedRuntime<bool, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Float32: return makeTypedRuntime<float, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Float64: return makeTypedRuntime<double, false>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::String: return makeTypedRuntime<std::string, false>(serverConfig, config, redis, alarmPublisher, generation);
    }
    break;
  case Shape::Array:
    switch (config.type) {
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Float32: return makeTypedRuntime<float, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Float64: return makeTypedRuntime<double, true>(serverConfig, config, redis, alarmPublisher, generation);
    case PrimitiveType::Boolean:
    case PrimitiveType::String:
      break;
    }
    break;
  }

  throw std::runtime_error("unsupported runtime type");
}

}  // namespace redis_pvxs_ioc
