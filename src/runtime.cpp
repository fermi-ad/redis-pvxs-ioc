#include "redis_pvxs_ioc/runtime.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
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
#include "redis_pvxs_ioc/ntndarray.h"
#include "redis_pvxs_ioc/util.h"
#include "redis_pvxs_ioc/xrm_hash_reader.h"

namespace redis_pvxs_ioc {
namespace {

bool sameRouteTarget(const RouteConfig& lhs, const RouteConfig& rhs) {
  return lhs.backend == rhs.backend && lhs.key == rhs.key;
}

bool sameRouteTarget(const RouteConfig& lhs, const ConfirmConfig& rhs) {
  return lhs.backend == rhs.backend && lhs.key == rhs.key;
}

std::string batchHealthError(const RedisAdapter::ReaderStatus& current,
                             const RedisAdapter::StreamBatchMetadata& metadata) {
  if (!current.connected) return "Source reader disconnected";
  if (current.readRejections > metadata.readRejections)
    return "Source read rejected; waiting for valid data";
  return {};
}

struct SourceTimestamp {
  uint64_t nanoseconds = 0;
  bool valid = false;
  std::string error;
};

struct XrmMetadata {
  bool present = false;
  std::string session;
  uint64_t connection = 0, batch = 0, sampleRateHz = 0, sampleCount = 0;
  uint32_t channel = 0, sampleBytes = 0;
  std::string sourceValidity = "unknown";
  bool acqTimeValid = false, acqTimePresent = false, acqTimeParsed = false;
};

struct XrmSequenceCounters {
  uint64_t missingBatches = 0, discontinuities = 0, sessionChanges = 0;
  uint64_t connectionChanges = 0, sequenceDiscontinuities = 0;
};

void saturatingAdd(uint64_t& value, uint64_t increment) {
  value = increment > UINT64_MAX - value ? UINT64_MAX : value + increment;
}

bool parseUnsignedDecimal(const std::string& text, uint64_t& output) {
  if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
    return false;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

uint32_t channelFromSubKey(const std::string& key) {
  const auto separator = key.rfind(':');
  const auto name = key.substr(separator == std::string::npos ? 0 : separator + 1);
  uint64_t channel = 0;
  if (name.size() > 2 && name.compare(0, 2, "CH") == 0 &&
      parseUnsignedDecimal(name.substr(2), channel) && channel >= 1 && channel <= 32)
    return static_cast<uint32_t>(channel);
  return 0;
}

bool parseXrmMetadata(const RedisAdapter::Attrs& fields, XrmMetadata& output) {
  const auto schema = fields.find("schema");
  const auto session = fields.find("session");
  const auto connection = fields.find("connection");
  const auto batch = fields.find("batch");
  const auto channel = fields.find("channel");
  const auto sampleRate = fields.find("sample_rate_hz");
  const auto count = fields.find("sample_count");
  const auto width = fields.find("sample_bytes");
  const auto sourceValidity = fields.find("source_validity");
  const auto timeValidity = fields.find("acq_time_valid");
  if (schema == fields.end() || schema->second != "fermi-ad/xrm-waveform:1" ||
      session == fields.end() || session->second.empty() || session->second.size() > 128 ||
      !std::all_of(session->second.begin(), session->second.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
      }) ||
      connection == fields.end() || batch == fields.end() || channel == fields.end() ||
      sampleRate == fields.end() || count == fields.end() || width == fields.end() ||
      sourceValidity == fields.end() || (sourceValidity->second != "unknown" &&
          sourceValidity->second != "valid" && sourceValidity->second != "invalid") ||
      timeValidity == fields.end() || (timeValidity->second != "0" && timeValidity->second != "1"))
    return false;

  XrmMetadata result;
  uint64_t channelNumber = 0, sampleWidth = 0;
  if (!parseUnsignedDecimal(connection->second, result.connection) || !result.connection ||
      !parseUnsignedDecimal(batch->second, result.batch) || !result.batch ||
      !parseUnsignedDecimal(channel->second, channelNumber) || !channelNumber || channelNumber > 32 ||
      !parseUnsignedDecimal(sampleRate->second, result.sampleRateHz) || !result.sampleRateHz ||
      result.sampleRateHz > 100000000 ||
      !parseUnsignedDecimal(count->second, result.sampleCount) || !result.sampleCount ||
      !parseUnsignedDecimal(width->second, sampleWidth) || !sampleWidth || sampleWidth > 4)
    return false;

  result.present = true;
  result.session = session->second;
  result.channel = static_cast<uint32_t>(channelNumber);
  result.sampleBytes = static_cast<uint32_t>(sampleWidth);
  result.sourceValidity = sourceValidity->second;
  result.acqTimeValid = timeValidity->second == "1";
  const auto time = fields.find("acq_time_ns");
  result.acqTimePresent = time != fields.end();
  uint64_t ignored = 0;
  result.acqTimeParsed = result.acqTimePresent && parseUnsignedDecimal(time->second, ignored);
  output = std::move(result);
  return true;
}

SourceTimestamp xrmTimestamp(const RedisAdapter::Attrs& fields) {
  uint64_t nanoseconds = 0;
  const auto time = fields.find("acq_time_ns");
  const bool haveTimestamp = time != fields.end() && parseUnsignedDecimal(time->second, nanoseconds);
  if (!haveTimestamp) nanoseconds = 0;

  SourceTimestamp result{nanoseconds, true, {}};
  const auto validity = fields.find("acq_time_valid");
  if (validity == fields.end() || (validity->second != "0" && validity->second != "1"))
    result = {nanoseconds, false, "Invalid XRM acquisition time validity"};
  else if (validity->second == "0") result = {nanoseconds, false, "XRM acquisition time unavailable or invalid"};
  else if (!haveTimestamp) result = {0, false, "Invalid XRM acquisition time"};

  const auto sourceValidity = fields.find("source_validity");
  std::string sourceError;
  if (sourceValidity != fields.end() && sourceValidity->second == "invalid")
    sourceError = "XRM source reports invalid waveform";
  else if (sourceValidity != fields.end() && sourceValidity->second == "unknown")
    sourceError = "XRM source validity is unknown";
  if (!sourceError.empty()) {
    if (!result.error.empty()) result.error += "; ";
    result.error += sourceError;
    result.valid = false;
  }
  return result;
}

// The protocol describes integer sample storage as little-endian bytes. The
// configured PV type supplies signedness; no native-endian memcpy is used.
template <typename T>
bool decodeXrmWaveform(const RedisAdapter::Attrs& fields, const XrmMetadata& metadata,
                       std::vector<T>& output, uint64_t maxBytes) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 4);
  const auto payload = fields.find("_");
  if (metadata.sampleBytes != sizeof(T) ||
      payload == fields.end() || payload->second.empty() || payload->second.size() > maxBytes ||
      payload->second.size() % sizeof(T) != 0 || payload->second.size() / sizeof(T) != metadata.sampleCount)
    return false;

  std::vector<T> decoded;
  decoded.reserve(payload->second.size() / sizeof(T));
  for (size_t offset = 0; offset < payload->second.size(); offset += sizeof(T)) {
    uint64_t bits = 0;
    for (size_t byte = 0; byte < sizeof(T); ++byte)
      bits |= static_cast<uint64_t>(static_cast<unsigned char>(payload->second[offset + byte])) << (byte * 8);
    if constexpr (std::is_signed_v<T>) {
      constexpr auto widthBits = sizeof(T) * 8;
      const auto value = (bits & (uint64_t{1} << (widthBits - 1)))
          ? static_cast<int64_t>(bits) - static_cast<int64_t>(uint64_t{1} << widthBits)
          : static_cast<int64_t>(bits);
      decoded.push_back(static_cast<T>(value));
    } else decoded.push_back(static_cast<T>(bits));
  }
  output = std::move(decoded);
  return true;
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

class NDArrayRuntime final : public PVRuntimeBase, public std::enable_shared_from_this<NDArrayRuntime> {
public:
  NDArrayRuntime(const ServerConfig& serverConfig, PVConfig config,
                 std::shared_ptr<RedisAdapter> redis, std::shared_ptr<AlarmPublisher> publisher,
                 std::shared_ptr<RuntimeStats> stats, uint64_t generation, uint64_t payloadLimit, uint32_t probeMs)
      : config_(std::move(config)), redis_(std::move(redis)), alarmPublisher_(std::move(publisher)),
        stats_(std::move(stats)), maxFrameBytes_(std::min(config_.maxFrameBytes, payloadLimit)),
        fullName_(fullPVName(serverConfig, config_)), generation_(generation), probeMs_(probeMs),
        pv_(pvxs::server::SharedPV::buildReadonly()) {}

  ~NDArrayRuntime() override { deactivate("runtime destroyed"); }

  void initialize() {
    auto initial = createEmptyNTNDArray();
    const auto snapshot = redis_->getStreamSnapshot(config_.read.key);
    if (snapshot.present()) {
      try {
        const auto frame = parseNDArrayFrame(snapshot.fields, RA_Time(snapshot.id).value, maxFrameBytes_);
        initial = buildNTNDArrayValue(frame);
        lastUniqueId_ = frame.uniqueId; haveGoodFrame_ = true;
        sample_.accept(snapshot.id, 1);
        currentAlarm_ = {epicsSevNone, epicsAlarmNone, ""};
      } catch (const std::exception& error) {
        ++pendingInvalid_;
        sample_.reject(std::string(error.what()).substr(0, 512));
        currentAlarm_ = {epicsSevInvalid, epicsAlarmUDF, std::string(error.what()).substr(0, 512)};
        setNTNDArrayInvalidAlarm(initial, currentAlarm_.message);
      }
    }
    prototype_ = initial.cloneEmpty();
    pv_.open(initial);
    if (alarmPublisher_) alarmRegistration_ = alarmPublisher_->prepare(fullName_, currentAlarm_);
    auto weak = weak_from_this();
    RedisAdapter::SubscriptionOptions selection;
    selection.afterId = snapshot.id; selection.probeMs = probeMs_;
    std::lock_guard<std::mutex> guard(mutex_);
    reader_ = redis_->subscribeStreamWithMetadata(config_.read.key,
        [weak](const auto&, const auto&, const RedisAdapter::StreamBatch& data, const auto& metadata) {
          if (const auto self = weak.lock()) self->handleRead(data, metadata);
        }, selection);
  }

  const PVConfig& config() const override { return config_; }
  const std::string& fullName() const override { return fullName_; }
  pvxs::server::SharedPV& sharedPV() override { return pv_; }
  bool structurallyCompatible(const PVConfig& config) const override { return sameReaderTopology(config_, config); }

  class Update final : public PVRuntimeUpdate {
  public:
    std::shared_ptr<NDArrayRuntime> runtime;
    PVConfig config;
    uint64_t generation;
    std::shared_ptr<AlarmPublisher> publisher;
    std::shared_ptr<AlarmPublisher::Registration> registration;
    void commit() noexcept override {
      std::lock_guard<std::mutex> guard(runtime->mutex_);
      std::swap(runtime->config_, config); runtime->generation_ = generation;
      if (registration != runtime->alarmRegistration_) {
        if (runtime->alarmRegistration_) runtime->alarmRegistration_->retire();
        runtime->alarmPublisher_.swap(publisher);
        runtime->alarmRegistration_.swap(registration);
      }
    }
    void refresh() override {
      std::lock_guard<std::mutex> guard(runtime->mutex_);
      if (runtime->alive_) runtime->publishAlarmLocked();
    }
  };

  std::unique_ptr<PVRuntimeUpdate> prepareReconfigure(const PVConfig& config, uint64_t generation,
      const std::shared_ptr<AlarmPublisher>& publisher = {}) override {
    auto update = std::make_unique<Update>();
    update->runtime = shared_from_this(); update->config = config; update->generation = generation;
    std::lock_guard<std::mutex> guard(mutex_);
    if (!alive_ || !sameReaderTopology(config_, config)) throw std::runtime_error("NTNDArray runtime changed during preparation");
    update->publisher = publisher ? publisher : alarmPublisher_;
    update->registration = update->publisher == alarmPublisher_ ? alarmRegistration_
        : update->publisher->prepare(fullName_, currentAlarm_);
    return update;
  }
  void reconfigure(const PVConfig& config, uint64_t generation) override {
    auto update = prepareReconfigure(config, generation); update->commit(); update->refresh();
  }
  void activate() noexcept override {
    std::lock_guard<std::mutex> guard(mutex_);
    if (committed_ || !alive_) return;
    committed_ = true;
    stats_->ndarrayInvalidFrames += pendingInvalid_; stats_->ndarraySkippedFrames += pendingSkipped_;
    stats_->ndarrayDiscontinuities += pendingDiscontinuities_;
    pendingInvalid_ = pendingSkipped_ = pendingDiscontinuities_ = 0;
    if (alarmRegistration_) alarmRegistration_->activate();
  }
  void deactivate(const std::string&) override {
    RedisAdapter::ReaderHandle retired;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (!alive_) return;
      alive_ = false; committed_ = false;
      if (alarmRegistration_) alarmRegistration_->retire();
      retired = std::move(reader_);
    }
    retired.reset(); // Retire only this subscription, including when another PV reads the same key.
    if (pv_.isOpen()) pv_.close();
  }

  std::vector<SourceStatus> sourceHealth(SourceClock::time_point now) override {
    std::lock_guard<std::mutex> guard(mutex_);
    auto status = assessSource(sample_, reader_.status(), config_, fullName_, config_.read,
                               "read", alive_ && committed_, probeMs_, now);
    if (alive_ && committed_ && status.error != healthError_) {
      healthError_ = status.error;
      currentAlarm_ = status.ready ? AlarmState{epicsSevNone, epicsAlarmNone, ""}
                                   : AlarmState{epicsSevInvalid, epicsAlarmUDF, status.error};
      auto value = pv_.fetch();
      if (status.ready) {
        value["alarm.severity"] = int32_t(epicsSevNone); value["alarm.status"] = int32_t(epicsAlarmNone);
        value["alarm.message"] = "";
      } else setNTNDArrayInvalidAlarm(value, status.error);
      pv_.post(value); publishAlarmLocked();
    }
    return {std::move(status)};
  }

private:
  void publishAlarmLocked() {
    if (alarmRegistration_) {
      alarmRegistration_->update(currentAlarm_);
      if (alive_ && committed_) alarmRegistration_->activate();
    }
  }
  bool acceptEpochLocked(uint64_t epoch, RedisAdapter::ReaderStatus* current = nullptr) {
    if (!alive_) return false;
    auto status = reader_.status();
    const bool matches = status.epoch == epoch;
    if (current) *current = std::move(status);
    if (matches) return true;
    ++sample_.staleCallbacks; return false;
  }
  void handleRead(const RedisAdapter::StreamBatch& data, const RedisAdapter::StreamBatchMetadata& metadata) {
    const auto epoch = metadata.epoch;
    for (const auto& item : data) {
      try {
        const auto frame = parseNDArrayFrame(item.second, RA_Time(item.first).value, maxFrameBytes_);
        // Owned PVXS arrays use the exact Type opened by this runtime.
        auto value = buildNTNDArrayValue(frame, prototype_);
        std::lock_guard<std::mutex> guard(mutex_);
        RedisAdapter::ReaderStatus current;
        if (!acceptEpochLocked(epoch, &current)) return;
        const bool newEpoch = sample_.epoch != epoch;
        const auto gap = haveGoodFrame_ && !newEpoch ? assessNDArrayFrameGap(lastUniqueId_, frame.uniqueId, config_.maxFrameGap)
                                       : NDArrayFrameGap{};
        if (committed_) {
          stats_->ndarraySkippedFrames += gap.skipped;
          stats_->ndarrayDiscontinuities += gap.discontinuity ? 1u : 0u;
        } else {
          pendingSkipped_ += gap.skipped;
          pendingDiscontinuities_ += gap.discontinuity ? 1u : 0u;
        }
        lastUniqueId_ = frame.uniqueId; haveGoodFrame_ = true;
        sample_.accept(item.first, epoch); sample_.acceptedReadRejections = metadata.readRejections;
        healthError_ = batchHealthError(current, metadata);
        currentAlarm_ = healthError_.empty() ? AlarmState{epicsSevNone, epicsAlarmNone, ""}
            : AlarmState{epicsSevInvalid, epicsAlarmUDF, healthError_};
        if (!healthError_.empty()) setNTNDArrayInvalidAlarm(value, healthError_);
        pv_.post(value);
        publishAlarmLocked();
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!acceptEpochLocked(epoch)) return;
        if (committed_) ++stats_->ndarrayInvalidFrames; else ++pendingInvalid_;
        currentAlarm_ = {epicsSevInvalid, epicsAlarmUDF, std::string(error.what()).substr(0, 512)};
        sample_.reject(currentAlarm_.message, epoch);
        auto value = pv_.fetch(); // Keep pixels, dimensions, ID and acquisition timestamp from the last good frame.
        setNTNDArrayInvalidAlarm(value, currentAlarm_.message);
        pv_.post(value);
        publishAlarmLocked();
      }
    }
  }

  mutable std::mutex mutex_;
  PVConfig config_;
  std::shared_ptr<RedisAdapter> redis_;
  std::shared_ptr<AlarmPublisher> alarmPublisher_;
  std::shared_ptr<AlarmPublisher::Registration> alarmRegistration_;
  std::shared_ptr<RuntimeStats> stats_;
  const uint64_t maxFrameBytes_;
  std::string fullName_;
  uint64_t generation_;
  const uint32_t probeMs_;
  pvxs::server::SharedPV pv_;
  pvxs::Value prototype_;
  RedisAdapter::ReaderHandle reader_;
  SourceSampleState sample_;
  std::string healthError_;
  bool alive_ = true, committed_ = false, haveGoodFrame_ = false;
  int32_t lastUniqueId_ = 0;
  uint64_t pendingInvalid_ = 0, pendingSkipped_ = 0, pendingDiscontinuities_ = 0;
  AlarmState currentAlarm_{epicsSevInvalid, epicsAlarmUDF, "no valid frame"};
};

std::shared_ptr<PVRuntimeBase> makeNDArrayRuntime(const ServerConfig& serverConfig, const PVConfig& config,
    const RedisBackendRegistry& backends, const std::shared_ptr<AlarmPublisher>& publisher,
    const std::shared_ptr<RuntimeStats>& stats, uint64_t generation, uint64_t payloadLimit, uint32_t probeMs) {
  auto runtime = std::make_shared<NDArrayRuntime>(serverConfig, config,
      resolveBackend(backends, config.read.backend, fullPVName(serverConfig, config), "read route"),
      publisher, stats, generation, payloadLimit, probeMs);
  runtime->initialize();
  return runtime;
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
               std::shared_ptr<OperationQueue> operations, OperationLimitsConfig limits,
               uint32_t readProbeMs, uint32_t confirmProbeMs, std::optional<RedisConfig> xrmBackend)
      : config_(std::move(config)), protocol_(config_.streamProtocol),
        expectedChannel_(channelFromSubKey(config_.read.key)), readRedis_(std::move(readRedis)),
        writeRedis_(std::move(writeRedis)), confirmRedis_(std::move(confirmRedis)),
        alarmPublisher_(std::move(alarmPublisher)), fullName_(fullPVName(serverConfig, config_)),
        generation_(generation), operations_(std::move(operations)), limits_(limits),
        confirmationMs_(config_.confirm ? config_.confirm->timeoutMs : 0),
        readProbeMs_(readProbeMs), confirmProbeMs_(confirmProbeMs),
        xrmBackend_(std::move(xrmBackend)),
        pv_(config_.write ? pvxs::server::SharedPV::buildMailbox() : pvxs::server::SharedPV::buildReadonly()) {}

  ~TypedRuntime() override { deactivate("runtime destroyed"); }

  void initialize() {
    if constexpr (Array) lastRaw_ = initialVectorOr<T>(config_.initialValue);
    else lastRaw_ = initialScalarOr<T>(config_.initialValue, T{});
    auto weak = this->weak_from_this();
    RedisAdapter::StreamSnapshot snapshot;
    if (protocol_ == StreamProtocol::XrmV1) {
      if (!xrmBackend_) throw std::runtime_error("xrm_v1 requires its Redis backend configuration");
      xrmReader_ = std::make_unique<XrmHashReader>(*xrmBackend_, config_.read.key, limits_.maxPayloadBytes, 20,
          [weak](const auto&, const auto&, const RedisAdapter::StreamBatch& data, const auto& metadata) {
            if (const auto self = weak.lock()) self->handleRead(data, true, false, metadata);
          });
      snapshot = xrmReader_->snapshot();
    } else snapshot = readRedis_->getStreamSnapshot(config_.read.key);
    readCursor_ = snapshot.id;
    ValueType decoded{};
    XrmMetadata xrm;
    const auto timestamp = sourceTimestamp(snapshot.fields, snapshot.id);
    if (snapshot.present() && decode(snapshot.fields, decoded, &xrm) &&
        (protocol_ == StreamProtocol::XrmV1 || timestamp.valid)) {
      lastRaw_ = std::move(decoded);
      if (protocol_ == StreamProtocol::XrmV1) {
        lastXrm_ = std::move(xrm);
        lastXrmCursor_ = snapshot.id;
      }
      lastTimestampNs_ = timestamp.nanoseconds;
      sourceValid_ = timestamp.valid;
      sourceError_ = timestamp.error;
      if (protocol_ == StreamProtocol::XrmV1 && sourceValid_) {
        sourceError_ = "Waiting for a new XRM publication after baseline";
        healthError_ = sourceError_;
        readSample_.reject(sourceError_);
      } else if (sourceValid_) readSample_.accept(snapshot.id, 1);
      else readSample_.reject(sourceError_);
    } else {
      sourceError_ = snapshot.present() ? "Invalid Redis snapshot" : snapshot.rejected ? "Redis snapshot rejected"
                                                                                     : "No source data; using initial fallback";
      if (snapshot.present() || snapshot.rejected) readSample_.reject(sourceError_);
    }
    auto initial = createInitialValue(config_);
    if (protocol_ == StreamProtocol::XrmV1) {
      using pvxs::Member;
      using pvxs::TypeCode;
      auto type = pvxs::TypeDef(initial);
      type += {Member(TypeCode::Struct, "xrm", "fermi-ad:xrm-waveform-meta:1.0", {
          Member(TypeCode::Bool, "present"), Member(TypeCode::String, "session"),
          Member(TypeCode::UInt64, "connection"), Member(TypeCode::UInt64, "batch"),
          Member(TypeCode::UInt32, "channel"), Member(TypeCode::UInt64, "sampleRateHz"),
          Member(TypeCode::UInt64, "sampleCount"), Member(TypeCode::UInt32, "sampleBytes"),
          Member(TypeCode::String, "sourceValidity"), Member(TypeCode::Bool, "acqTimeValid"),
          Member(TypeCode::Bool, "acqTimePresent"), Member(TypeCode::Bool, "acqTimeParsed"),
          Member(TypeCode::UInt64, "missingBatches"), Member(TypeCode::UInt64, "discontinuities"),
          Member(TypeCode::UInt64, "sessionChanges"), Member(TypeCode::UInt64, "connectionChanges"),
          Member(TypeCode::UInt64, "sequenceDiscontinuities")})};
      initial = type.create();
    }
    const auto initialAlarm = populateValue(initial);
    if (alarmPublisher_) alarmRegistration_ = alarmPublisher_->prepare(fullName_, initialAlarm);
    pv_.open(initial);
    if (config_.write) {
      pv_.onPut([weak](pvxs::server::SharedPV&, std::unique_ptr<pvxs::server::ExecOp>&& op,
                       pvxs::Value&& value) {
        if (const auto self = weak.lock()) self->handlePut(std::move(op), std::move(value));
        else op->error("runtime unavailable");
      });
    }
    const bool readConfirms = config_.confirm && sameRouteTarget(config_.read, *config_.confirm);
    RedisAdapter::SubscriptionOptions selection;
    selection.afterId = readCursor_; selection.probeMs = readProbeMs_;
    std::lock_guard<std::mutex> guard(mutex_);
    if (xrmReader_) xrmReader_->start();
    else readReader_ = readRedis_->subscribeStreamWithMetadata(config_.read.key,
          [weak, readConfirms](const auto&, const auto&, const RedisAdapter::StreamBatch& data, const auto& metadata) {
            if (const auto self = weak.lock()) self->handleRead(data, true, readConfirms, metadata);
          }, selection);
    if (config_.confirm && !readConfirms) {
      const auto confirmation = confirmRedis_->getStreamSnapshot(config_.confirm->key);
      ValueType raw{};
      if (confirmation.present() && decode(confirmation.fields, raw) && RA_Time(confirmation.id).ok())
        confirmSample_.accept(confirmation.id, 1);
      else if (confirmation.present()) confirmSample_.reject("Invalid confirmation snapshot");
      selection.afterId = confirmation.id; selection.probeMs = confirmProbeMs_;
      confirmReader_ = confirmRedis_->subscribeStreamWithMetadata(config_.confirm->key,
          [weak](const auto&, const auto&, const RedisAdapter::StreamBatch& data, const auto& metadata) {
            if (const auto self = weak.lock()) self->handleRead(data, false, true, metadata);
          }, selection);
    }
  }

  const PVConfig& config() const override { return config_; }
  const std::string& fullName() const override { return fullName_; }
  pvxs::server::SharedPV& sharedPV() override { return pv_; }
  bool structurallyCompatible(const PVConfig& config) const override { return sameReaderTopology(config_, config); }
  void activate() noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    committed_ = true;
    if (alarmRegistration_) alarmRegistration_->activate();
  }

  class Update final : public PVRuntimeUpdate {
  public:
    std::shared_ptr<TypedRuntime> runtime;
    PVConfig config;
    uint64_t generation = 0;
    std::shared_ptr<const std::string> cancellation;
    std::shared_ptr<AlarmPublisher> publisher;
    std::shared_ptr<AlarmPublisher::Registration> alarm;
    void commit() noexcept override {
      std::lock_guard<std::mutex> lock(runtime->mutex_);
      const auto& old = runtime->config_.transform;
      const bool sameTransform = old.has_value() == config.transform.has_value() &&
          (!old || (old->scale == config.transform->scale && old->offset == config.transform->offset));
      if (!sameTransform) {
        ++runtime->commandEpoch_;
        runtime->failPendingLocked(cancellation);
      }
      static_assert(std::is_nothrow_swappable_v<PVConfig>);
      if (alarm != runtime->alarmRegistration_) {
        if (runtime->alarmRegistration_) runtime->alarmRegistration_->retire();
        runtime->alarmPublisher_.swap(publisher);
        runtime->alarmRegistration_.swap(alarm);
        // The new registration stays inactive until refresh/read processing
        // supplies the latest state; preparation-time samples are not emitted.
      }
      std::swap(runtime->config_, config);
      runtime->generation_ = generation;
    }
    void refresh() override { runtime->refreshMetadata(); }
  };

  std::unique_ptr<PVRuntimeUpdate> prepareReconfigure(const PVConfig& config, uint64_t generation,
      const std::shared_ptr<AlarmPublisher>& publisher = {}) override {
    auto result = std::make_unique<Update>();
    result->runtime = this->shared_from_this();
    result->config = config;
    result->generation = generation;
    result->cancellation = std::make_shared<const std::string>("write transform changed during reload");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!alive_ || !sameReaderTopology(config_, config)) throw std::runtime_error("runtime changed during preparation");
    // Validate metadata/value construction now, but do not post a stale sample
    // at commit: readback can advance while the remaining generation is staged.
    auto preview = pv_.fetch();
    const auto state = populateValue(preview, config);
    result->publisher = publisher ? publisher : alarmPublisher_;
    result->alarm = result->publisher == alarmPublisher_ ? alarmRegistration_
        : result->publisher->prepare(fullName_, state);
    return result;
  }

  void reconfigure(const PVConfig& config, uint64_t generation) override {
    auto update = prepareReconfigure(config, generation);
    update->commit();
    update->refresh();
  }

  void refreshMetadata() {
    AlarmState state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_) return;
      auto value = pv_.fetch();
      const auto priorPayload = value["value"].clone();
      value.unmark();
      state = populateValue(value);
      // Reload changes presentation, not acquisition. fetch() carries cached
      // marks, and populateValue() assigns even unchanged fields. Preserve a
      // real transform change, but do not publish the retained sample again.
      if (samePresentedPayload(priorPayload, value["value"])) value["value"].unmark();
      value["timeStamp"].unmark();
      if (protocol_ == StreamProtocol::XrmV1) value["xrm"].unmark();
      pv_.post(value);
      recordAlarmLocked(state);
    }
  }

  void deactivate(const std::string& reason) override {
    if (!alive_.exchange(false)) return;
    RedisAdapter::ReaderHandle retiredRead, retiredConfirm;
    std::unique_ptr<XrmHashReader> retiredXrm;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++commandEpoch_;
      if (alarmRegistration_) alarmRegistration_->retire();
      failPendingLocked(std::make_shared<const std::string>(reason));
      retiredRead = std::move(readReader_); retiredConfirm = std::move(confirmReader_);
      retiredXrm = std::move(xrmReader_);
    }
    retiredRead.reset(); retiredConfirm.reset();
    if (retiredXrm) retiredXrm->reset();
    if (pv_.isOpen()) pv_.close();
  }

  std::vector<SourceStatus> sourceHealth(SourceClock::time_point now) override {
    std::lock_guard<std::mutex> guard(mutex_);
    const bool sameConfirmation = config_.confirm && sameRouteTarget(config_.read, *config_.confirm);
    auto read = assessSource(readSample_, readStatusLocked(), config_, fullName_, config_.read,
        sameConfirmation ? "read-confirm" : "read", alive_ && committed_, readProbeMs_, now);
    const auto lagError = read.ready ? xrmLagErrorLocked() : std::string{};
    const bool lagStale = !lagError.empty();
    if (lagStale) {
      if (!pipelineLagStale_) ++readSample_.staleTransitions;
      read.staleTransitions = readSample_.staleTransitions;
      read.stale = true; read.fresh = false; read.ready = false;
      read.state = "stale"; read.error = lagError;
    }
    pipelineLagStale_ = lagStale;
    std::vector<SourceStatus> result;
    const auto confirmationEpoch = sameConfirmation ? read.reader.epoch : confirmReader_.status().epoch;
    if (alive_ && committed_) {
      for (auto& item : pendingPuts_) {
        auto& pending = *item.second;
        if (!pending.done && pending.sourceEpoch != confirmationEpoch) {
          pending.done = true;
          pending.error = std::make_shared<const std::string>("confirmation source epoch changed; not retried");
          pending.cv.notify_all();
        }
      }
      if (read.error != healthError_) {
        healthError_ = read.error;
        auto value = pv_.fetch();
        const auto alarm = populateValue(value);
        pv_.post(value); recordAlarmLocked(alarm);
      }
    }
    result.push_back(std::move(read));
    if (config_.confirm && !sameConfirmation) {
      const RouteConfig route{config_.confirm->backend, config_.confirm->key};
      result.push_back(assessSource(confirmSample_, confirmReader_.status(), config_, fullName_, route,
          "confirm", alive_ && committed_, confirmProbeMs_, now));
    }
    return result;
  }

private:
  static bool samePresentedPayload(const pvxs::Value& prior, const pvxs::Value& next) {
    const auto equal = [](T lhs, T rhs) {
      // Presentation equality must retain signed-zero/NaN representation
      // changes and must not reuse the tolerance for write confirmations.
      if constexpr (std::is_floating_point_v<T>)
        return std::memcmp(&lhs, &rhs, sizeof(T)) == 0;
      else return lhs == rhs;
    };
    if constexpr (Array) {
      const auto before = prior.as<pvxs::shared_array<const T>>();
      const auto after = next.as<pvxs::shared_array<const T>>();
      return before.size() == after.size() && std::equal(before.begin(), before.end(), after.begin(), equal);
    } else return equal(prior.as<T>(), next.as<T>());
  }

  struct PendingPut {
    uint64_t id = 0;
    ValueType expectedRaw{};
    std::string afterId;
    uint64_t sourceEpoch = 0;
    bool done = false;
    std::shared_ptr<const std::string> error;
    std::condition_variable cv;
  };

  bool decode(const RedisAdapter::Attrs& fields, ValueType& raw, XrmMetadata* metadata = nullptr) const {
    if (protocol_ == StreamProtocol::XrmV1) {
      if constexpr (Array && std::is_integral_v<T> && !std::is_same_v<T, bool> && sizeof(T) <= 4) {
        XrmMetadata parsed;
        if (!parseXrmMetadata(fields, parsed) || (expectedChannel_ && parsed.channel != expectedChannel_) ||
            !decodeXrmWaveform(fields, parsed, raw, limits_.maxPayloadBytes))
          return false;
        if (metadata) *metadata = std::move(parsed);
        return true;
      } else return false;
    }
    if constexpr (Array) return RedisAdapter::decodeArray(fields, raw, limits_.maxPayloadBytes);
    else return RedisAdapter::decodeScalar(fields, raw, limits_.maxPayloadBytes);
  }

  SourceTimestamp sourceTimestamp(const RedisAdapter::Attrs& fields, const std::string& streamId) const {
    if (protocol_ == StreamProtocol::XrmV1) return xrmTimestamp(fields);
    const auto legacy = RA_Time(streamId);
    if (!legacy.ok()) return {0, false, "Invalid Redis source timestamp"};
    return {static_cast<uint64_t>(legacy.value), true, {}};
  }

  void applyXrmMetadata(pvxs::Value& value) const {
    value["xrm.present"] = lastXrm_.present;
    value["xrm.session"] = lastXrm_.session;
    value["xrm.connection"] = lastXrm_.connection;
    value["xrm.batch"] = lastXrm_.batch;
    value["xrm.channel"] = lastXrm_.channel;
    value["xrm.sampleRateHz"] = lastXrm_.sampleRateHz;
    value["xrm.sampleCount"] = lastXrm_.sampleCount;
    value["xrm.sampleBytes"] = lastXrm_.sampleBytes;
    value["xrm.sourceValidity"] = lastXrm_.sourceValidity;
    value["xrm.acqTimeValid"] = lastXrm_.acqTimeValid;
    value["xrm.acqTimePresent"] = lastXrm_.acqTimePresent;
    value["xrm.acqTimeParsed"] = lastXrm_.acqTimeParsed;
    value["xrm.missingBatches"] = xrmCounters_.missingBatches;
    value["xrm.discontinuities"] = xrmCounters_.discontinuities;
    value["xrm.sessionChanges"] = xrmCounters_.sessionChanges;
    value["xrm.connectionChanges"] = xrmCounters_.connectionChanges;
    value["xrm.sequenceDiscontinuities"] = xrmCounters_.sequenceDiscontinuities;
  }

  void acceptXrmMetadata(const XrmMetadata& next) {
    if (lastXrm_.present) {
      if (lastXrm_.session != next.session) {
        saturatingAdd(xrmCounters_.sessionChanges, 1);
        saturatingAdd(xrmCounters_.discontinuities, 1);
      } else if (lastXrm_.connection != next.connection) {
        saturatingAdd(xrmCounters_.connectionChanges, 1);
        saturatingAdd(xrmCounters_.discontinuities, 1);
      } else if (next.batch > lastXrm_.batch) {
        saturatingAdd(xrmCounters_.missingBatches, next.batch - lastXrm_.batch - 1);
      } else {
        saturatingAdd(xrmCounters_.sequenceDiscontinuities, 1);
        saturatingAdd(xrmCounters_.discontinuities, 1);
      }
    }
    lastXrm_ = next;
  }

  AlarmState populateValue(pvxs::Value& value, const PVConfig& config) {
    const auto present = applyForwardTransform(config, lastRaw_);
    if constexpr (Array) assignArrayValue(value, present);
    else assignScalarValue(value, present);
    applyStandardMetadata(value, config);
    if (protocol_ == StreamProtocol::XrmV1) applyXrmMetadata(value);
    if (lastTimestampNs_) applyTimestamp(value, lastTimestampNs_);
    else {
      value["timeStamp.secondsPastEpoch"] = static_cast<int64_t>(0);
      value["timeStamp.nanoseconds"] = static_cast<int32_t>(0);
      value["timeStamp.userTag"] = static_cast<int32_t>(0);
    }
    AlarmState state{epicsSevNone, epicsAlarmNone, ""};
    if (!healthError_.empty()) state = {epicsSevInvalid, epicsAlarmUDF, healthError_};
    else if (!sourceValid_) state = {epicsSevInvalid, epicsAlarmUDF, sourceError_};
    else if constexpr (!Array && std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
      const int prior = value["alarm.status"].as<int>();
      state = evaluateNumericAlarm(config, static_cast<double>(present), prior);
    }
    applyAlarmFields(value, config, state);
    return state;
  }

  AlarmState populateValue(pvxs::Value& value) { return populateValue(value, config_); }

  void recordAlarmLocked(const AlarmState& state) {
    if (alarmRegistration_) {
      alarmRegistration_->update(state);
      if (alive_ && committed_) alarmRegistration_->activate();
    }
  }

  void handleRead(const RedisAdapter::StreamBatch& data, bool updateValue, bool canConfirm,
                  const RedisAdapter::StreamBatchMetadata& metadata) {
    // Evaluate every observed sample for alarms and confirmations, including
    // intermediate values in a batch. Confirmation data never changes readback.
    for (const auto& entry : data) {
      if (!alive_) return;
      ValueType raw{};
      XrmMetadata xrm;
      const auto timestamp = sourceTimestamp(entry.second, entry.first);
      if (!decode(entry.second, raw, &xrm) || (protocol_ == StreamProtocol::Legacy && !timestamp.valid)) {
        const auto error = protocol_ == StreamProtocol::XrmV1 ? "Invalid XRM waveform schema or payload"
                                                              : "Invalid Redis payload or source timestamp";
        if (!markInvalid(error, updateValue, metadata.epoch)) return;
        continue;
      }
      if (!handleRawUpdate(raw, timestamp.nanoseconds, updateValue, canConfirm, entry.first, &metadata,
                           timestamp.valid, timestamp.error, &xrm)) return;
    }
  }

  bool acceptEpochLocked(bool read, uint64_t epoch, RedisAdapter::ReaderStatus* current = nullptr) {
    if (!alive_) return false;
    auto status = read ? readStatusLocked() : confirmReader_.status();
    const bool matches = status.epoch == epoch;
    if (current) *current = std::move(status);
    if (matches) return true;
    ++(read ? readSample_ : confirmSample_).staleCallbacks; return false;
  }

  bool markInvalid(const std::string& error, bool updateValue, uint64_t epoch) {
    AlarmState state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!acceptEpochLocked(updateValue, epoch)) return false;
      (updateValue ? readSample_ : confirmSample_).reject(error, epoch);
      if (!updateValue) return true;
      sourceValid_ = false;
      sourceError_ = error;
      healthError_.clear();
      auto value = pv_.fetch();
      state = populateValue(value);
      pv_.post(value);
      recordAlarmLocked(state);
    }
    return true;
  }

  bool handleRawUpdate(const ValueType& raw, uint64_t timestamp, bool updateValue,
                       bool canConfirm = false, const std::string& streamId = {},
                       const RedisAdapter::StreamBatchMetadata* metadata = nullptr,
                       bool sourceValid = true, const std::string& sourceError = {},
                       const XrmMetadata* xrm = nullptr) {
    AlarmState state;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!alive_) return false;
      const auto epoch = metadata ? metadata->epoch : 0;
      RedisAdapter::ReaderStatus current;
      if (epoch) {
        if (!acceptEpochLocked(updateValue, epoch, &current)) return false;
        auto& sample = updateValue ? readSample_ : confirmSample_;
        if (sourceValid && protocol_ == StreamProtocol::XrmV1) {
          const auto proof = xrmReader_->freshnessProof();
          if (proof != SourceClock::time_point{}) sample.accept(streamId, epoch, proof);
          else sample.reject("Waiting for a new XRM publication after baseline", epoch);
        } else if (sourceValid) sample.accept(streamId, epoch);
        else sample.reject(sourceError, epoch);
        sample.acceptedReadRejections = metadata->readRejections;
      }
      if (canConfirm && sourceValid) {
        for (auto& item : pendingPuts_) {
          auto& pending = *item.second;
          if (!pending.done && pending.sourceEpoch != epoch) {
            pending.done = true;
            pending.error = std::make_shared<const std::string>("confirmation source epoch changed; not retried");
            pending.cv.notify_all();
          } else if (!pending.done && RedisAdapter::compareStreamIds(streamId, pending.afterId) > 0 &&
              valuesEqual(pending.expectedRaw, raw)) {
            pending.done = true;
            pending.cv.notify_all();
          }
        }
      }
      if (!updateValue) return true;
      lastRaw_ = raw;
      if (protocol_ == StreamProtocol::XrmV1 && xrm) {
        acceptXrmMetadata(*xrm);
        lastXrmCursor_ = streamId;
      }
      lastTimestampNs_ = timestamp;
      sourceValid_ = sourceValid;
      sourceError_ = sourceError;
      if (epoch) {
        healthError_ = batchHealthError(current, *metadata);
        const auto lagError = sourceValid ? xrmLagErrorLocked() : std::string{};
        if (!lagError.empty()) {
          if (!pipelineLagStale_) ++readSample_.staleTransitions;
          if (healthError_.empty()) healthError_ = lagError;
        }
        pipelineLagStale_ = !lagError.empty();
      }
      auto value = pv_.fetch();
      state = populateValue(value);
      // Serialize value and alarm-state updates with commit and retirement.
      pv_.post(value);
      recordAlarmLocked(state);
    }
    return true;
  }

  void failPendingLocked(const std::shared_ptr<const std::string>& reason) {
    for (auto& item : pendingPuts_) {
      item.second->done = true;
      item.second->error = reason;
      item.second->cv.notify_all();
    }
  }

  RedisAdapter::ReaderStatus readStatusLocked() const {
    if (protocol_ == StreamProtocol::XrmV1)
      return xrmReader_ ? xrmReader_->status() : RedisAdapter::ReaderStatus{};
    return readReader_.status();
  }

  std::string xrmLagErrorLocked() const {
    if (protocol_ != StreamProtocol::XrmV1 || !xrmReader_ || !lastXrm_.present) return {};
    if (xrmReader_->status().observedCursor != lastXrmCursor_)
      return "XRM pipeline is awaiting its observed latest publication";
    const auto proof = xrmReader_->freshnessProof();
    if (proof == SourceClock::time_point{}) return "Waiting for a new XRM publication after baseline";
    if (SourceClock::now() - proof >= std::chrono::milliseconds(config_.sourceHealth.staleAfterMs))
      return "XRM publication freshness cannot be established";
    return {};
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
      uint64_t confirmationEpoch;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        confirmationEpoch = (sameRouteTarget(current.read, *current.confirm) ? readReader_ : confirmReader_).status().epoch;
      }
      const auto snapshot = confirmRedis_->getStreamSnapshot(current.confirm->key);
      if (!snapshot.connected) { request->error("confirmation backend unavailable"); return; }
      pending = std::make_shared<PendingPut>();
      pending->expectedRaw = raw;
      pending->afterId = snapshot.id;
      pending->sourceEpoch = confirmationEpoch;
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
        request->error("write no longer authorized at dispatch"); return;
      }
      if (request->stopped()) return;
      if (pending) {
        if (pending->sourceEpoch != (sameRouteTarget(current.read, *current.confirm) ? readReader_ : confirmReader_).status().epoch) {
          request->error("confirmation source epoch changed before dispatch"); return;
        }
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
    if (error) request->error(*error);
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
  const StreamProtocol protocol_;
  const uint32_t expectedChannel_;
  std::shared_ptr<RedisAdapter> readRedis_, writeRedis_, confirmRedis_;
  std::shared_ptr<AlarmPublisher> alarmPublisher_;
  std::shared_ptr<AlarmPublisher::Registration> alarmRegistration_;
  std::string fullName_;
  uint64_t generation_ = 0;
  std::atomic<uint64_t> commandEpoch_{0};
  std::shared_ptr<OperationQueue> operations_;
  const OperationLimitsConfig limits_;
  const int confirmationMs_;
  const uint32_t readProbeMs_, confirmProbeMs_;
  const std::optional<RedisConfig> xrmBackend_;
  pvxs::server::SharedPV pv_;
  std::atomic<bool> alive_{true};       // false once retired
  std::atomic<bool> committed_{false};  // true after staging commits
  RedisAdapter::ReaderHandle readReader_, confirmReader_;
  std::unique_ptr<XrmHashReader> xrmReader_;
  SourceSampleState readSample_, confirmSample_;
  std::string healthError_;
  std::string readCursor_;
  ValueType lastRaw_{};
  XrmMetadata lastXrm_;
  std::string lastXrmCursor_;
  XrmSequenceCounters xrmCounters_;
  bool pipelineLagStale_ = false;
  uint64_t lastTimestampNs_ = 0;
  bool sourceValid_ = false;
  std::string sourceError_;
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
                                                OperationLimitsConfig limits, const RedisBackendConfigs& readerPolicies) {
  const auto fullName = fullPVName(serverConfig, config);
  std::optional<RedisConfig> xrmBackend;
  if (config.streamProtocol == StreamProtocol::XrmV1) {
    const auto backend = readerPolicies.find(config.read.backend);
    if (backend == readerPolicies.end()) throw std::runtime_error("xrm_v1 requires its Redis backend configuration");
    xrmBackend = backend->second;
  }
  auto runtime = std::make_shared<TypedRuntime<T, Array>>(
      serverConfig,
      config,
      resolveBackend(redisBackends, config.read.backend, fullName, "read route"),
      config.write ? resolveBackend(redisBackends, config.write->backend, fullName, "write route") : nullptr,
      config.confirm ? resolveBackend(redisBackends, config.confirm->backend, fullName, "confirm route") : nullptr,
      alarmPublisher,
      generation, std::move(operations), limits,
      readerPolicies.count(config.read.backend) ? readerPolicies.at(config.read.backend).readerProbeMs : 1000,
      config.confirm && readerPolicies.count(config.confirm->backend) ? readerPolicies.at(config.confirm->backend).readerProbeMs : 1000,
      std::move(xrmBackend));
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
                                                OperationLimitsConfig limits,
                                                std::shared_ptr<RuntimeStats> stats,
                                                const RedisBackendConfigs& readerPolicies) {
  if (config.streamProtocol == StreamProtocol::XrmV1) {
    const bool supportedType = config.type == PrimitiveType::Int8 || config.type == PrimitiveType::UInt8 ||
        config.type == PrimitiveType::Int16 || config.type == PrimitiveType::UInt16 ||
        config.type == PrimitiveType::Int32 || config.type == PrimitiveType::UInt32;
    if (config.kind != PVKind::Value || config.shape != Shape::Array || !supportedType || config.write || config.confirm)
      throw std::runtime_error("xrm_v1 requires a read-only 8-, 16-, or 32-bit integer array");
    if (!config.sourceHealth.staleAfterMs)
      throw std::runtime_error("xrm_v1 requires a nonzero source freshness deadline");
  }
  if (config.kind == PVKind::NTNDArray)
    return makeNDArrayRuntime(serverConfig, config, redisBackends, alarmPublisher,
                              stats ? stats : std::make_shared<RuntimeStats>(), generation, limits.maxPayloadBytes,
                              readerPolicies.count(config.read.backend) ? readerPolicies.at(config.read.backend).readerProbeMs : 1000);
  if (!operations) {
    static auto defaultOperations = std::make_shared<OperationQueue>();
    operations = defaultOperations;
  }
  switch (config.shape) {
  case Shape::Scalar:
    switch (config.type) {
    case PrimitiveType::Boolean: return makeTypedRuntime<bool, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Float32: return makeTypedRuntime<float, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Float64: return makeTypedRuntime<double, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::String: return makeTypedRuntime<std::string, false>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    }
    break;
  case Shape::Array:
    switch (config.type) {
    case PrimitiveType::Int8: return makeTypedRuntime<int8_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt8: return makeTypedRuntime<uint8_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int16: return makeTypedRuntime<int16_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt16: return makeTypedRuntime<uint16_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int32: return makeTypedRuntime<int32_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt32: return makeTypedRuntime<uint32_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Int64: return makeTypedRuntime<int64_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::UInt64: return makeTypedRuntime<uint64_t, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Float32: return makeTypedRuntime<float, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Float64: return makeTypedRuntime<double, true>(serverConfig, config, redisBackends, alarmPublisher, generation, operations, limits, readerPolicies);
    case PrimitiveType::Boolean:
    case PrimitiveType::String:
      break;
    }
    break;
  }

  throw std::runtime_error("unsupported runtime type");
}

}  // namespace redis_pvxs_ioc
