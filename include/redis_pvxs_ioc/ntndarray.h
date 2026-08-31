#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pvxs/data.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

inline constexpr const char kNTNDArrayRedisSchema[] = "fermi-ad/redis-adapter/ntndarray:1";
inline constexpr const char kNTNDArrayPayloadField[] = "_";

enum class NDColorMode {
  Mono = 0,
  RGB1 = 2,
  RGB2 = 3,
  RGB3 = 4,
};

enum class NDTimeSource : uint32_t {
  Unknown = 0,
  Host = 1,
  CameraCorrelated = 2,
  PTP = 3,
  ExternalTrigger = 4,
};

struct NDTimeProvenance {
  NDTimeSource source = NDTimeSource::Unknown;
  std::optional<uint64_t> uncertaintyNs;
  std::optional<uint64_t> cameraTimestamp;
  std::optional<uint64_t> cameraTimestampHz;
};

struct NDArrayFrame {
  PrimitiveType dataType = PrimitiveType::UInt8;
  std::vector<int32_t> shape;
  NDColorMode colorMode = NDColorMode::Mono;
  std::vector<uint8_t> payload;
  int32_t uniqueId = 0;
  uint64_t dataTimestampNs = 0;
  NDTimeProvenance provenance;
};

using NDArrayAttrs = std::unordered_map<std::string, std::string>;

size_t elementSize(PrimitiveType type);
NDArrayFrame parseNDArrayFrame(const NDArrayAttrs& attrs,
                               int64_t streamTimestampNs,
                               uint64_t maxFrameBytes);
pvxs::Value createEmptyNTNDArray(const std::string& reason = "no valid frame");
pvxs::Value buildNTNDArrayValue(const NDArrayFrame& frame);
pvxs::Value buildNTNDArrayValue(const NDArrayFrame& frame, const pvxs::Value& prototype);
void setNTNDArrayInvalidAlarm(pvxs::Value& value, const std::string& reason);

}  // namespace redis_pvxs_ioc
