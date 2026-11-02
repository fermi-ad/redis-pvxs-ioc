#include "redis_pvxs_ioc/source_time.h"

#include <charconv>
#include <stdexcept>

namespace redis_pvxs_ioc {
namespace {

int64_t parseNanoseconds(const std::string& text) {
  auto begin = text.data();
  const auto end = begin + text.size();
  // from_chars accepts '-' but not '+'. Both signs are part of this decimal
  // contract; neither whitespace nor a second sign is permitted.
  if (begin != end && *begin == '+') ++begin;
  if (begin == end || (text.front() == '+' && (*begin < '0' || *begin > '9')))
    throw std::invalid_argument("source time must be signed int64 decimal text");
  int64_t result = 0;
  const auto parsed = std::from_chars(begin, end, result, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != end)
    throw std::invalid_argument("source time must be signed int64 decimal text");
  return result;
}

}  // namespace

DecodedSourceTime decodeSourceTime(const SourceTimeConfig& config,
                                  const RedisAdapter::Attrs& fields,
                                  const std::string& streamId) {
  switch (config.mode) {
  case SourceTimeMode::LegacyRedisId: {
    const RA_Time timestamp(streamId);
    if (!timestamp.ok()) throw std::invalid_argument("invalid legacy Redis source timestamp");
    return {timestamp.value, true};
  }
  case SourceTimeMode::Unavailable:
    return {};
  case SourceTimeMode::UnixNanosecondsV1: {
    DecodedSourceTime result;
    const auto valid = fields.find(config.validField);
    if (valid != fields.end()) {
      if (valid->second != "0" && valid->second != "1")
        throw std::invalid_argument("source time validity must be literal 0 or 1");
      result.valid = valid->second == "1";
    }
    const auto timestamp = fields.find(config.field);
    if (timestamp != fields.end()) result.nanoseconds = parseNanoseconds(timestamp->second);
    else if (result.valid) throw std::invalid_argument("valid source time requires a timestamp");
    return result;
  }
  }
  throw std::invalid_argument("unsupported source time mode");
}

void applySourceTimestamp(pvxs::Value& value, const std::optional<int64_t>& nanoseconds) {
  constexpr int64_t perSecond = 1'000'000'000;
  const int64_t timestamp = nanoseconds.value_or(0);
  auto seconds = timestamp / perSecond;
  auto remainder = timestamp % perSecond;
  // Division truncates toward zero. Normalize negative times without negating
  // the input, which must also work for INT64_MIN.
  if (remainder < 0) {
    --seconds;
    remainder += perSecond;
  }
  value["timeStamp.secondsPastEpoch"] = seconds;
  value["timeStamp.nanoseconds"] = static_cast<int32_t>(remainder);
  value["timeStamp.userTag"] = static_cast<int32_t>(0);
}

std::string sourceTimeModeName(SourceTimeMode mode) {
  switch (mode) {
  case SourceTimeMode::LegacyRedisId: return "legacy-redis-id";
  case SourceTimeMode::UnixNanosecondsV1: return "unix-nanoseconds-v1";
  case SourceTimeMode::Unavailable: return "unavailable";
  }
  throw std::invalid_argument("unsupported source time mode");
}

}  // namespace redis_pvxs_ioc
