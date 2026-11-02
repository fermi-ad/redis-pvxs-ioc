#include "redis_pvxs_ioc/source_time.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <pvxs/nt.h>

using namespace redis_pvxs_ioc;

namespace {

const SourceTimeConfig kUnix{SourceTimeMode::UnixNanosecondsV1, "acquisition_time_ns", "acquisition_time_valid"};

template<typename F> bool throwsInvalid(F action) {
  try {
    action();
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}

void checkTimestamp(pvxs::Value& value, const std::optional<int64_t>& nanoseconds,
                    int64_t seconds, int32_t fraction) {
  value["timeStamp.userTag"] = 17;
  applySourceTimestamp(value, nanoseconds);
  assert(value["timeStamp.secondsPastEpoch"].as<int64_t>() == seconds);
  assert(value["timeStamp.nanoseconds"].as<int32_t>() == fraction);
  assert(value["timeStamp.userTag"].as<int32_t>() == 0);
}

}  // namespace

int main() {
  assert(sourceTimeModeName(SourceTimeMode::LegacyRedisId) == "legacy-redis-id");
  assert(sourceTimeModeName(SourceTimeMode::UnixNanosecondsV1) == "unix-nanoseconds-v1");
  assert(sourceTimeModeName(SourceTimeMode::Unavailable) == "unavailable");

  // Legacy IDs retain the adapter's original millisecond-plus-remainder
  // interpretation, including sequence components greater than one millisecond.
  const auto legacy = decodeSourceTime({}, {{"acquisition_time_ns", "bad"},
      {"acquisition_time_valid", "bad"}}, "1700000000123-1234567");
  assert(legacy.valid && legacy.nanoseconds == 1700000000124234567LL);
  for (const auto* id : {"0-0", "0", "bad", "1-", "-1-2", "1.0-1", "9223372036854-775808"})
    assert(throwsInvalid([&] { decodeSourceTime({}, {}, id); }));

  // Unix mode never interprets the Redis cursor as acquisition time.
  for (const auto& item : {
      std::pair<const char*, int64_t>{"0", 0}, {"+0", 0}, {"-0", 0},
      {"1", 1}, {"+1", 1}, {"-1", -1},
      {"1700000000123456789", 1700000000123456789LL},
      {"-1700000000123456789", -1700000000123456789LL},
      {"9223372036854775807", std::numeric_limits<int64_t>::max()},
      {"+9223372036854775807", std::numeric_limits<int64_t>::max()},
      {"-9223372036854775808", std::numeric_limits<int64_t>::min()}}) {
    const auto result = decodeSourceTime(kUnix, {{kUnix.field, item.first}, {kUnix.validField, "1"}}, "ignored");
    assert(result.valid && result.nanoseconds == item.second);
  }

  // A supplied time survives invalid or omitted validity. Missing time is
  // unknown unless the producer explicitly claims that it is valid.
  for (const auto& fields : {RedisAdapter::Attrs{}, RedisAdapter::Attrs{{kUnix.validField, "0"}}}) {
    const auto result = decodeSourceTime(kUnix, fields, "100-0");
    assert(!result.valid && !result.nanoseconds);
  }
  for (const auto& fields : {RedisAdapter::Attrs{{kUnix.field, "-7"}},
      RedisAdapter::Attrs{{kUnix.field, "-7"}, {kUnix.validField, "0"}}}) {
    const auto result = decodeSourceTime(kUnix, fields, "100-0");
    assert(!result.valid && result.nanoseconds == -7);
  }
  assert(throwsInvalid([&] { decodeSourceTime(kUnix, {{kUnix.validField, "1"}}, "100-0"); }));

  for (const auto* invalid : {"", "2", "-1", "+1", "01", "true", "false", "1.0", " 1", "1 "})
    assert(throwsInvalid([&] { decodeSourceTime(kUnix, {{kUnix.field, "0"}, {kUnix.validField, invalid}}, "1-0"); }));

  for (const auto& invalid : {std::string{}, std::string("+"), std::string("-"), std::string("+-1"),
      std::string("--1"), std::string("++1"), std::string(" 1"), std::string("1 "), std::string("1\n"),
      std::string("1.0"), std::string("1e9"), std::string("0x10"), std::string("nan"),
      std::string("9223372036854775808"), std::string("+9223372036854775808"),
      std::string("-9223372036854775809"), std::string("1\0", 2)}) {
    for (const auto& validity : {RedisAdapter::Attrs{}, RedisAdapter::Attrs{{kUnix.validField, "0"}},
        RedisAdapter::Attrs{{kUnix.validField, "1"}}}) {
      auto fields = validity;
      fields[kUnix.field] = invalid;
      assert(throwsInvalid([&] { decodeSourceTime(kUnix, fields, "1-0"); }));
    }
  }

  const auto unavailable = decodeSourceTime({SourceTimeMode::Unavailable, {}, {}},
      {{kUnix.field, "bad"}, {kUnix.validField, "bad"}}, "bad");
  assert(!unavailable.valid && !unavailable.nanoseconds);

  auto value = pvxs::nt::NTScalar{pvxs::TypeCode::Float64A}.create();
  checkTimestamp(value, 1700000000123456789LL, 1700000000LL, 123456789);
  checkTimestamp(value, 1700000000123456788LL, 1700000000LL, 123456788); // Backward source time is preserved.
  checkTimestamp(value, 0, 0, 0); // Epoch zero must never fall back to the host clock.
  checkTimestamp(value, 1, 0, 1);
  checkTimestamp(value, -1, -1, 999999999);
  checkTimestamp(value, -999999999LL, -1, 1);
  checkTimestamp(value, -1000000000LL, -1, 0);
  checkTimestamp(value, -1000000001LL, -2, 999999999);
  checkTimestamp(value, -1700000000123456789LL, -1700000001LL, 876543211);
  checkTimestamp(value, std::numeric_limits<int64_t>::min(), -9223372037LL, 145224192);
  checkTimestamp(value, std::numeric_limits<int64_t>::max(), 9223372036LL, 854775807);
  checkTimestamp(value, std::nullopt, 0, 0);

  std::cout << "source time: exact signed timestamps, optional validity, strict decoding and legacy compatibility passed\n";
}
