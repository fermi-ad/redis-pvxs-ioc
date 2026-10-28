#include "redis_pvxs_ioc/xrm_acquisition.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace redis_pvxs_ioc {
namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

bool safeKeyPart(const std::string& value) {
  return !value.empty() && value.size() <= 128 &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
      });
}

const char* validityName(XRMSourceValidity validity) {
  switch (validity) {
    case XRMSourceValidity::Unknown: return "unknown";
    case XRMSourceValidity::Valid: return "valid";
    case XRMSourceValidity::Invalid: return "invalid";
  }
  throw std::invalid_argument("unrecognized source validity");
}
size_t validateConfig(const XRMStreamConfig& config) {
  require(!config.channels.empty() && config.channels.size() <= 32,
          "select 1 to 32 channels");
  unsigned previous = 0;
  for (const auto channel : config.channels) {
    require(channel > previous && channel <= 32, "channels must be unique, ascending and in 1..32");
    previous = channel;
  }
  require(config.sampleBytes >= 1 && config.sampleBytes <= 4,
          "sample storage width must be 1 to 4 bytes");
  require(config.sampleRateHz > 0 && config.sampleRateHz <= 100000000,
          "sample rate must be in 1..100000000 Hz");
  const auto stride = config.channels.size() * config.sampleBytes;
  require(config.samplesPerBatch > 0 && config.samplesPerBatch <= kXRMMaxBatchBytes / stride,
          "sample batch exceeds byte bound or is empty");
  return config.samplesPerBatch * stride;
}
}  // namespace

XRMFrameAssembler::XRMFrameAssembler(XRMStreamConfig config) : config_(std::move(config)) {
  batchBytes_ = validateConfig(config_);
  buffered_.reserve(batchBytes_);
}

void XRMFrameAssembler::consume(std::string_view input, const Sink& sink) {
  if (!sink) throw std::invalid_argument("missing batch sink");
  while (!input.empty()) {
    const auto count = std::min(input.size(), remainingBytes());
    buffered_.append(input.data(), count);
    input.remove_prefix(count);
    if (buffered_.size() != batchBytes_) continue;

    XRMBatch batch;
    batch.samples = config_.samplesPerBatch;
    batch.channels.reserve(config_.channels.size());
    for (auto channel : config_.channels) {
      batch.channels.push_back({channel, std::string(batch.samples * config_.sampleBytes, '\0')});
    }
    const size_t stride = config_.channels.size() * config_.sampleBytes;
    for (size_t sample = 0; sample < batch.samples; ++sample) {
      const auto* source = buffered_.data() + sample * stride;
      for (size_t channel = 0; channel < batch.channels.size(); ++channel) {
        std::memcpy(batch.channels[channel].bytes.data() + sample * config_.sampleBytes,
                    source + channel * config_.sampleBytes, config_.sampleBytes);
      }
    }
    buffered_.clear();
    sink(std::move(batch));
  }
}

size_t XRMFrameAssembler::reset() noexcept {
  const auto discarded = buffered_.size();
  buffered_.clear();
  return discarded;
}

std::vector<XRMRedisRecord> makeXRMRedisRecords(
    const XRMStreamConfig& config, const XRMBatch& batch,
    const XRMAcquisitionMetadata& metadata, const std::string& base,
    const std::string& keyPrefix, const std::string& session,
    uint64_t connection, uint64_t batchSequence) {
  // Validate the same bounds even for caller-constructed batches/configs.
  validateConfig(config);
  require(safeKeyPart(base) && safeKeyPart(keyPrefix) && safeKeyPart(session),
          "invalid base, key prefix or session identifier");
  require(connection > 0 && batchSequence > 0, "connection and batch identifiers must be positive");
  require(!metadata.timeValid || metadata.unixTimeNs.has_value(), "valid source time requires a Unix timestamp");
  require(batch.samples == config.samplesPerBatch && batch.channels.size() == config.channels.size(),
          "batch does not match stream configuration");
  std::vector<XRMRedisRecord> result;
  result.reserve(batch.channels.size());
  for (size_t i = 0; i < batch.channels.size(); ++i) {
    const auto& channel = batch.channels[i];
    require(channel.channel == config.channels[i] &&
            channel.bytes.size() == batch.samples * config.sampleBytes,
            "channel payload does not match stream configuration");
    XRMRedisFields fields{
      {"_", channel.bytes}, {"schema", kXRMWaveformSchema},
      {"sample_bytes", std::to_string(config.sampleBytes)},
      {"sample_rate_hz", std::to_string(config.sampleRateHz)},
      {"sample_count", std::to_string(batch.samples)},
      {"channel", std::to_string(channel.channel)},
      {"session", session}, {"connection", std::to_string(connection)},
      {"batch", std::to_string(batchSequence)},
      {"source_validity", validityName(metadata.sourceValidity)},
      {"acq_time_valid", metadata.timeValid ? "1" : "0"},
    };
    if (metadata.unixTimeNs) fields.emplace_back("acq_time_ns", std::to_string(*metadata.unixTimeNs));
    result.push_back({"{" + base + "}:" + keyPrefix + ":CH" + std::to_string(channel.channel), std::move(fields)});
  }
  return result;
}

}  // namespace redis_pvxs_ioc
