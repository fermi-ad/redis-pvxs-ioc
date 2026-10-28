#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace redis_pvxs_ioc {

// This model consists of interleaved, little-endian sample words in ascending
// selected-channel order. The device must already be configured to match the
// model. This object does not configure the instrument or infer its format.
struct XRMStreamConfig {
  std::vector<unsigned> channels;
  unsigned sampleBytes = 0;
  uint64_t sampleRateHz = 0;
  size_t samplesPerBatch = 0;
};

inline constexpr size_t kXRMMaxBatchBytes = 8u * 1024u * 1024u;
inline constexpr char kXRMWaveformSchema[] = "fermi-ad/xrm-waveform:1";

// A channel payload is a sequence of unchanged little-endian sample words.
struct XRMChannelPayload {
  unsigned channel;
  std::string bytes;
};
struct XRMBatch {
  size_t samples;
  std::vector<XRMChannelPayload> channels;
};

class XRMFrameAssembler {
public:
  explicit XRMFrameAssembler(XRMStreamConfig config);
  using Sink = std::function<void(XRMBatch&&)>;

  // Processes arbitrarily split input with at most one batch buffered. A
  // completed batch is removed before invoking sink; if sink throws, the
  // caller must discard any unconsumed portion of this input.
  void consume(std::string_view input, const Sink& sink);
  // Discard the incomplete batch on a socket disconnect. Never combine data
  // from different connections, even when the byte count is sample-aligned.
  size_t reset() noexcept;
  size_t bufferedBytes() const noexcept { return buffered_.size(); }
  size_t remainingBytes() const noexcept { return batchBytes_ - buffered_.size(); }
  size_t batchBytes() const noexcept { return batchBytes_; }
  const XRMStreamConfig& config() const noexcept { return config_; }

private:
  XRMStreamConfig config_;
  size_t batchBytes_ = 0;
  std::string buffered_;
};

enum class XRMSourceValidity { Unknown, Valid, Invalid };
struct XRMAcquisitionMetadata {
  // Only populate for a source whose timestamp is known to be Unix ns. An
  // absent value means unknown; Redis IDs and receipt clocks are not a source.
  std::optional<uint64_t> unixTimeNs;
  // A source may supply a timestamp while marking it invalid. Preserve both.
  bool timeValid = false;
  XRMSourceValidity sourceValidity = XRMSourceValidity::Unknown;
};

using XRMRedisFields = std::vector<std::pair<std::string, std::string>>;
struct XRMRedisRecord {
  std::string key;
  XRMRedisFields fields;
};

// Keys use the redis-adapter {base}:subkey convention. Every record carries
// the same session, connection and batch identifiers for correlation across
// channels. A separate publisher assigns monotonically ordered Redis IDs.
std::vector<XRMRedisRecord> makeXRMRedisRecords(
    const XRMStreamConfig& config, const XRMBatch& batch,
    const XRMAcquisitionMetadata& metadata, const std::string& base,
    const std::string& keyPrefix, const std::string& session,
    uint64_t connection, uint64_t batchSequence);

}  // namespace redis_pvxs_ioc
