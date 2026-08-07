#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <alarm.h>

#include "redis_pvxs_ioc/ntndarray.h"

using namespace redis_pvxs_ioc;

namespace {

NDArrayAttrs envelope(const std::string& type,
                      const std::string& shape,
                      std::string payload) {
  return {
      {"schema", kNTNDArrayRedisSchema},
      {kNTNDArrayPayloadField, std::move(payload)},
      {"data_type", type},
      {"shape", shape},
      {"color_mode", "mono"},
      {"unique_id", "42"},
      {"time_source", "camera_correlated"},
      {"time_uncertainty_ns", "125000"},
      {"camera_timestamp", "123456789"},
      {"camera_timestamp_hz", "1000000000"},
  };
}

constexpr int64_t kStreamTimestampNs = 1785945600123456789LL;

bool throws(const std::function<void()>& fn) {
  try {
    fn();
    return false;
  } catch (...) {
    return true;
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, size_t>> types{
      {"0", 1}, {"1", 1}, {"2", 2}, {"3", 2}, {"4", 4},
      {"5", 4}, {"6", 8}, {"7", 8}, {"8", 4}, {"9", 8}};
  for (const auto& type : types) {
    const auto frame = parseNDArrayFrame(
        envelope(type.first, "[2, 3]", std::string(6u * type.second, '\0')),
        kStreamTimestampNs, 1024);
    assert(frame.payload.size() == 6u * type.second);
    assert(frame.shape == std::vector<int32_t>({2, 3}));
    assert(frame.dataTimestampNs == static_cast<uint64_t>(kStreamTimestampNs));
    assert(frame.provenance.source == NDTimeSource::CameraCorrelated);
    assert(frame.provenance.uncertaintyNs == 125000u);
    const auto value = buildNTNDArrayValue(frame);
    assert(value.idStartsWith("epics:nt/NTNDArray:1.0"));
    assert(value["uniqueId"].as<int32_t>() == 42);
    assert(value["dimension[0].size"].as<int32_t>() == 2);
    assert(value["dimension[1].size"].as<int32_t>() == 3);
    assert(value["compressedSize"].as<int64_t>() == static_cast<int64_t>(frame.payload.size()));
    assert(value["alarm.severity"].as<int32_t>() == epicsSevNone);
    assert(value["dataTimeStamp.secondsPastEpoch"].as<int64_t>() ==
           kStreamTimestampNs / 1'000'000'000LL);
    assert(value["attribute[1].name"].as<std::string>() == "AcquisitionTimeNs");
    assert(value["attribute[1].value"].as<uint64_t>() ==
           static_cast<uint64_t>(kStreamTimestampNs));
  }

  auto rgb1 = envelope("1", "[3, 4, 2]", std::string(24, '\1'));
  rgb1["color_mode"] = "rgb1";
  auto rgbValue = buildNTNDArrayValue(
      parseNDArrayFrame(rgb1, kStreamTimestampNs, 1024));
  assert(rgbValue["attribute[0].name"].as<std::string>() == "ColorMode");
  assert(rgbValue["attribute[0].value"].as<uint16_t>() == 2u);

  auto invalidRgb = rgb1;
  invalidRgb["shape"] = "[4, 4, 2]";
  assert(throws([&]() {
    parseNDArrayFrame(invalidRgb, kStreamTimestampNs, 1024);
  }));

  auto missing = envelope("1", "[2]", std::string(2, '\0'));
  missing.erase("schema");
  assert(throws([&]() {
    parseNDArrayFrame(missing, kStreamTimestampNs, 1024);
  }));
  auto badSchema = envelope("1", "[2]", std::string(2, '\0'));
  badSchema["schema"] = "other";
  assert(throws([&]() {
    parseNDArrayFrame(badSchema, kStreamTimestampNs, 1024);
  }));
  auto truncated = envelope("3", "[2]", std::string(3, '\0'));
  assert(throws([&]() {
    parseNDArrayFrame(truncated, kStreamTimestampNs, 1024);
  }));
  auto oversized = envelope("1", "[5]", std::string(5, '\0'));
  assert(throws([&]() {
    parseNDArrayFrame(oversized, kStreamTimestampNs, 4);
  }));
  auto overflow = envelope("9", "[2147483647,2147483647,2147483647]", "");
  assert(throws([&]() {
    parseNDArrayFrame(overflow, kStreamTimestampNs, UINT64_MAX);
  }));
  assert(throws([&]() {
    parseNDArrayFrame(envelope("1", "[1]", std::string(1, '\0')), 0, 1024);
  }));
  auto unknown = envelope("1", "[1]", std::string(1, '\0'));
  unknown["codec"] = "none";
  assert(!throws([&]() {
    parseNDArrayFrame(unknown, kStreamTimestampNs, 1024);
  }));

  const std::vector<uint16_t> endianInput{0x0102u, 0xa0b0u};
  auto little = envelope("3", "[2]", std::string("\x02\x01\xb0\xa0", 4));
  const auto endianFrame = parseNDArrayFrame(little, kStreamTimestampNs, 1024);
  const auto endianValue = buildNTNDArrayValue(endianFrame);
  const auto decoded = endianValue["value"].as<pvxs::shared_array<const uint16_t>>();
  assert(decoded.size() == endianInput.size());
  assert(decoded[0] == endianInput[0]);
  assert(decoded[1] == endianInput[1]);

  auto empty = createEmptyNTNDArray();
  assert(empty["alarm.severity"].as<int32_t>() == epicsSevInvalid);
  assert(empty["alarm.message"].as<std::string>() == "no valid frame");
  std::cout << "ntndarray tests passed\n";
  return 0;
}
