#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <RedisAdapter.hpp>
#include <pvxs/data.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct DecodedSourceTime {
  std::optional<int64_t> nanoseconds;
  bool valid = false;
};

// Malformed timing data rejects the sample with std::invalid_argument.
DecodedSourceTime decodeSourceTime(const SourceTimeConfig& config,
                                  const RedisAdapter::Attrs& fields,
                                  const std::string& streamId);

// No timestamp means unknown (zero); epoch zero is a real supplied timestamp.
void applySourceTimestamp(pvxs::Value& value, const std::optional<int64_t>& nanoseconds);
std::string sourceTimeModeName(SourceTimeMode mode);

}  // namespace redis_pvxs_ioc
