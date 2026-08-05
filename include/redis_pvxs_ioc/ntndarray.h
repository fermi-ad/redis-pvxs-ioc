#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <pvxs/data.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

inline constexpr const char kNTNDArrayRedisSchema[] = "redis-pvxs-ioc/ntndarray:1";

enum class NDColorMode {
  Mono = 0,
  RGB1 = 2,
  RGB2 = 3,
  RGB3 = 4,
};

struct NDArrayFrame {
  PrimitiveType dataType = PrimitiveType::UInt8;
  std::vector<int32_t> shape;
  NDColorMode colorMode = NDColorMode::Mono;
  std::vector<uint8_t> payload;
  int32_t uniqueId = 0;
  uint64_t dataTimestampNs = 0;
};

using NDArrayAttrs = std::unordered_map<std::string, std::string>;

size_t elementSize(PrimitiveType type);
NDArrayFrame parseNDArrayFrame(const NDArrayAttrs& attrs, uint64_t maxFrameBytes);
pvxs::Value createEmptyNTNDArray(const std::string& reason = "no valid frame");
pvxs::Value buildNTNDArrayValue(const NDArrayFrame& frame);
void setNTNDArrayInvalidAlarm(pvxs::Value& value, const std::string& reason);

}  // namespace redis_pvxs_ioc
