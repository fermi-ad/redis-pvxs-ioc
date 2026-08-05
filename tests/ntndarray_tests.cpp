#include <cassert>
#include <cmath>
#include <cstring>
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
                      std::string payload,
                      const std::string& order = "little") {
  return {
      {"schema", kNTNDArrayRedisSchema},
      {"payload", std::move(payload)},
      {"data_type", type},
      {"shape", shape},
      {"color_mode", "mono"},
      {"byte_order", order},
      {"unique_id", "42"},
      {"data_timestamp_ns", "1785945600123456789"},
  };
}

bool throws(const std::function<void()>& fn) {
  try {
    fn();
    return false;
  } catch (...) {
    return true;
  }
}

template <typename T>
std::string bytes(const std::vector<T>& values) {
  return std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T));
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, size_t>> types{
      {"int8", 1}, {"uint8", 1}, {"int16", 2}, {"uint16", 2}, {"int32", 4},
      {"uint32", 4}, {"int64", 8}, {"uint64", 8}, {"float32", 4}, {"float64", 8}};
  for (const auto& type : types) {
    const auto frame = parseNDArrayFrame(envelope(type.first, "[2, 3]", std::string(6u * type.second, '\0')), 1024);
    assert(frame.payload.size() == 6u * type.second);
    assert(frame.shape == std::vector<int32_t>({2, 3}));
    const auto value = buildNTNDArrayValue(frame);
    assert(value.idStartsWith("epics:nt/NTNDArray:1.0"));
    assert(value["uniqueId"].as<int32_t>() == 42);
    assert(value["dimension[0].size"].as<int32_t>() == 2);
    assert(value["dimension[1].size"].as<int32_t>() == 3);
    assert(value["compressedSize"].as<int64_t>() == static_cast<int64_t>(frame.payload.size()));
    assert(value["alarm.severity"].as<int32_t>() == epicsSevNone);
  }

  auto rgb1 = envelope("uint8", "[3, 4, 2]", std::string(24, '\1'));
  rgb1["color_mode"] = "rgb1";
  auto rgbValue = buildNTNDArrayValue(parseNDArrayFrame(rgb1, 1024));
  assert(rgbValue["attribute[0].name"].as<std::string>() == "ColorMode");
  assert(rgbValue["attribute[0].value"].as<uint16_t>() == 2u);

  auto invalidRgb = rgb1;
  invalidRgb["shape"] = "[4, 4, 2]";
  assert(throws([&]() { parseNDArrayFrame(invalidRgb, 1024); }));

  auto missing = envelope("uint8", "[2]", std::string(2, '\0'));
  missing.erase("schema");
  assert(throws([&]() { parseNDArrayFrame(missing, 1024); }));
  auto badSchema = envelope("uint8", "[2]", std::string(2, '\0'));
  badSchema["schema"] = "other";
  assert(throws([&]() { parseNDArrayFrame(badSchema, 1024); }));
  auto truncated = envelope("uint16", "[2]", std::string(3, '\0'));
  assert(throws([&]() { parseNDArrayFrame(truncated, 1024); }));
  auto oversized = envelope("uint8", "[5]", std::string(5, '\0'));
  assert(throws([&]() { parseNDArrayFrame(oversized, 4); }));
  auto overflow = envelope("float64", "[2147483647,2147483647,2147483647]", "");
  assert(throws([&]() { parseNDArrayFrame(overflow, UINT64_MAX); }));
  auto unknown = envelope("uint8", "[1]", std::string(1, '\0'));
  unknown["codec"] = "none";
  assert(throws([&]() { parseNDArrayFrame(unknown, 1024); }));

  const std::vector<uint16_t> endianInput{0x0102u, 0xa0b0u};
  auto big = envelope("uint16", "[2]", std::string("\x01\x02\xa0\xb0", 4), "big");
  const auto endianFrame = parseNDArrayFrame(big, 1024);
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
