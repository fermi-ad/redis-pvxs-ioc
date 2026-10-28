#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

#include "redis_pvxs_ioc/xrm_latest_reader.h"

namespace redis_pvxs_ioc {

namespace detail {
struct ParsedXrmTail {
  uint64_t connection = 0, batch = 0, sampleCount = 0, sampleRateHz = 0;
};

inline bool parseXrmTail(const XrmTailMetadata& tail, ParsedXrmTail& result) {
  const auto parse = [](const std::string& text, uint64_t& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
      return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
  };
  if (!tail.present || tail.session.empty() || tail.session.size() > 128 ||
      !std::all_of(tail.session.begin(), tail.session.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
      }) ||
      !parse(tail.connection, result.connection) || !result.connection ||
      !parse(tail.batch, result.batch) || !result.batch ||
      !parse(tail.sampleCount, result.sampleCount) || !result.sampleCount ||
      !parse(tail.sampleRate, result.sampleRateHz) || !result.sampleRateHz || result.sampleRateHz > 100000000)
    return false;
  return true;
}
}  // namespace detail

// A valid observed tail from another session or connection is known to be
// newer than the current processed value, even if the time lag is unknown.
inline bool xrmTailIdentityChanged(std::string_view session, uint64_t connection, const XrmTailMetadata& tail) {
  detail::ParsedXrmTail parsed;
  return !session.empty() && connection && detail::parseXrmTail(tail, parsed) &&
      (tail.session != session || parsed.connection != connection);
}

// Estimate the producer-batch distance in nominal milliseconds. This is not
// an acquisition timestamp or an exact queue age. No estimate is made across
// sessions, connections, or different sample counts/rates.
inline bool xrmNominalPipelineLagExceeds(std::string_view session, uint64_t connection, uint64_t batch,
                                        uint64_t sampleCount, uint64_t sampleRateHz,
                                        const XrmTailMetadata& tail, uint32_t staleMs) {
  detail::ParsedXrmTail parsed;
  if (session.empty() || !connection || !batch || !sampleCount || !sampleRateHz || !tail.present || !staleMs ||
      !detail::parseXrmTail(tail, parsed) || tail.session != session || parsed.connection != connection ||
      parsed.batch < batch || parsed.sampleCount != sampleCount || parsed.sampleRateHz != sampleRateHz)
    return false;
  return static_cast<long double>(parsed.batch - batch) * static_cast<long double>(sampleCount) * 1000.0L >=
         static_cast<long double>(staleMs) * static_cast<long double>(sampleRateHz);
}

}  // namespace redis_pvxs_ioc
