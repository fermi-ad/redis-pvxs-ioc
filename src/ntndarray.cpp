#include "redis_pvxs_ioc/ntndarray.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <alarm.h>
#include <yaml-cpp/yaml.h>

#include <pvxs/nt.h>

#include "redis_pvxs_ioc/util.h"

namespace redis_pvxs_ioc {
namespace {

const std::string& required(const NDArrayAttrs& attrs, const char* name) {
  const auto it = attrs.find(name);
  if (it == attrs.end()) {
    throw std::runtime_error(std::string("missing field '") + name + "'");
  }
  return it->second;
}

template <typename T>
T parseUnsigned(const std::string& value, const char* name) {
  static_assert(std::is_unsigned_v<T>);
  T result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("invalid unsigned integer in '") + name + "'");
  }
  return result;
}

PrimitiveType parseDataType(const std::string& text) {
  if (text == "int8") return PrimitiveType::Int8;
  if (text == "uint8") return PrimitiveType::UInt8;
  if (text == "int16") return PrimitiveType::Int16;
  if (text == "uint16") return PrimitiveType::UInt16;
  if (text == "int32") return PrimitiveType::Int32;
  if (text == "uint32") return PrimitiveType::UInt32;
  if (text == "int64") return PrimitiveType::Int64;
  if (text == "uint64") return PrimitiveType::UInt64;
  if (text == "float32") return PrimitiveType::Float32;
  if (text == "float64") return PrimitiveType::Float64;
  throw std::runtime_error("unsupported data_type '" + text + "'");
}

NDColorMode parseColorMode(const std::string& text) {
  if (text == "mono") return NDColorMode::Mono;
  if (text == "rgb1") return NDColorMode::RGB1;
  if (text == "rgb2") return NDColorMode::RGB2;
  if (text == "rgb3") return NDColorMode::RGB3;
  throw std::runtime_error("unsupported color_mode '" + text + "'");
}

std::vector<int32_t> parseShape(const std::string& text) {
  YAML::Node root;
  try {
    root = YAML::Load(text);
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("invalid shape JSON: ") + ex.what());
  }
  if (!root.IsSequence() || root.size() < 1u || root.size() > 3u) {
    throw std::runtime_error("shape must contain one to three dimensions");
  }
  std::vector<int32_t> shape;
  shape.reserve(root.size());
  for (size_t index = 0; index < root.size(); ++index) {
    uint64_t size = 0;
    try {
      size = root[index].as<uint64_t>();
    } catch (const std::exception&) {
      throw std::runtime_error("shape dimensions must be positive integers");
    }
    if (size == 0u || size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::runtime_error("shape dimension is zero or exceeds int32");
    }
    shape.push_back(static_cast<int32_t>(size));
  }
  return shape;
}

bool hostIsLittleEndian() {
  const uint16_t marker = 1u;
  return *reinterpret_cast<const uint8_t*>(&marker) == 1u;
}

void validateColorShape(const NDColorMode mode, const std::vector<int32_t>& shape) {
  if (mode == NDColorMode::Mono) return;
  const size_t channelDimension = mode == NDColorMode::RGB1 ? 0u : mode == NDColorMode::RGB2 ? 1u : 2u;
  if (shape.size() <= channelDimension || shape[channelDimension] != 3) {
    throw std::runtime_error("RGB color mode requires a size-3 channel dimension in the matching layout");
  }
}

template <typename T>
void assignPixels(pvxs::Value& value, const char* member, const std::vector<uint8_t>& payload) {
  std::vector<T> pixels(payload.size() / sizeof(T));
  if (!payload.empty()) std::memcpy(pixels.data(), payload.data(), payload.size());
  pvxs::shared_array<T> array(pixels.begin(), pixels.end());
  value[member] = array.freeze();
}

void assignTimestamp(pvxs::Value value, const uint64_t timestampNs) {
  value["secondsPastEpoch"] = static_cast<int64_t>(timestampNs / 1'000'000'000ULL);
  value["nanoseconds"] = static_cast<int32_t>(timestampNs % 1'000'000'000ULL);
  value["userTag"] = 0;
}

}  // namespace

size_t elementSize(const PrimitiveType type) {
  switch (type) {
  case PrimitiveType::Int8:
  case PrimitiveType::UInt8: return 1u;
  case PrimitiveType::Int16:
  case PrimitiveType::UInt16: return 2u;
  case PrimitiveType::Int32:
  case PrimitiveType::UInt32:
  case PrimitiveType::Float32: return 4u;
  case PrimitiveType::Int64:
  case PrimitiveType::UInt64:
  case PrimitiveType::Float64: return 8u;
  case PrimitiveType::Boolean:
  case PrimitiveType::String: break;
  }
  throw std::runtime_error("non-numeric NTNDArray element type");
}

NDArrayFrame parseNDArrayFrame(const NDArrayAttrs& attrs, const uint64_t maxFrameBytes) {
  static const std::vector<std::string> allowed{
      "schema", "payload", "data_type", "shape", "color_mode", "byte_order",
      "unique_id", "data_timestamp_ns"};
  for (const auto& field : attrs) {
    if (std::find(allowed.begin(), allowed.end(), field.first) == allowed.end()) {
      throw std::runtime_error("unknown field '" + field.first + "'");
    }
  }
  if (required(attrs, "schema") != kNTNDArrayRedisSchema) {
    throw std::runtime_error("unsupported schema");
  }

  NDArrayFrame frame;
  frame.dataType = parseDataType(required(attrs, "data_type"));
  frame.shape = parseShape(required(attrs, "shape"));
  frame.colorMode = parseColorMode(required(attrs, "color_mode"));
  validateColorShape(frame.colorMode, frame.shape);
  const auto uniqueId = parseUnsigned<uint32_t>(required(attrs, "unique_id"), "unique_id");
  if (uniqueId > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
    throw std::runtime_error("unique_id exceeds NTNDArray int32 range");
  }
  frame.uniqueId = static_cast<int32_t>(uniqueId);
  frame.dataTimestampNs = parseUnsigned<uint64_t>(required(attrs, "data_timestamp_ns"), "data_timestamp_ns");
  if (frame.dataTimestampNs == 0u || frame.dataTimestampNs > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    throw std::runtime_error("data_timestamp_ns is outside the supported POSIX range");
  }

  uint64_t elements = 1u;
  for (const auto dimension : frame.shape) {
    const auto unsignedDimension = static_cast<uint64_t>(dimension);
    if (elements > std::numeric_limits<uint64_t>::max() / unsignedDimension) {
      throw std::runtime_error("shape element-count overflow");
    }
    elements *= unsignedDimension;
  }
  const auto width = static_cast<uint64_t>(elementSize(frame.dataType));
  if (elements > std::numeric_limits<uint64_t>::max() / width) {
    throw std::runtime_error("frame byte-count overflow");
  }
  const auto expectedBytes = elements * width;
  if (expectedBytes > maxFrameBytes) {
    throw std::runtime_error("frame exceeds max_frame_bytes");
  }
  const auto& payload = required(attrs, "payload");
  if (payload.size() != expectedBytes) {
    throw std::runtime_error("payload length does not match data_type and shape");
  }
  frame.payload.assign(payload.begin(), payload.end());

  const auto& byteOrder = required(attrs, "byte_order");
  if (byteOrder != "little" && byteOrder != "big") {
    throw std::runtime_error("byte_order must be 'little' or 'big'");
  }
  const bool sourceLittle = byteOrder == "little";
  if (width > 1u && sourceLittle != hostIsLittleEndian()) {
    for (uint64_t offset = 0; offset < expectedBytes; offset += width) {
      std::reverse(frame.payload.begin() + static_cast<ptrdiff_t>(offset),
                   frame.payload.begin() + static_cast<ptrdiff_t>(offset + width));
    }
  }
  return frame;
}

void setNTNDArrayInvalidAlarm(pvxs::Value& value, const std::string& reason) {
  value["alarm.severity"] = epicsSevInvalid;
  value["alarm.status"] = epicsAlarmUDF;
  value["alarm.message"] = reason;
  applyTimestamp(value);
}

pvxs::Value createEmptyNTNDArray(const std::string& reason) {
  auto value = pvxs::nt::NTNDArray{}.create();
  pvxs::shared_array<uint8_t> empty;
  value["value->ubyteValue"] = empty.freeze();
  value["codec.name"] = std::string("");
  value["compressedSize"] = static_cast<int64_t>(0);
  value["uncompressedSize"] = static_cast<int64_t>(0);
  value["uniqueId"] = static_cast<int32_t>(0);
  assignTimestamp(value["dataTimeStamp"], 0u);
  pvxs::shared_array<pvxs::Value> dimensions;
  value["dimension"] = dimensions.freeze();
  pvxs::shared_array<pvxs::Value> attributes;
  value["attribute"] = attributes.freeze();
  setNTNDArrayInvalidAlarm(value, reason);
  return value;
}

namespace {

pvxs::Value populateNTNDArrayValue(pvxs::Value value, const NDArrayFrame& frame) {
  switch (frame.dataType) {
  case PrimitiveType::Int8: assignPixels<int8_t>(value, "value->byteValue", frame.payload); break;
  case PrimitiveType::UInt8: assignPixels<uint8_t>(value, "value->ubyteValue", frame.payload); break;
  case PrimitiveType::Int16: assignPixels<int16_t>(value, "value->shortValue", frame.payload); break;
  case PrimitiveType::UInt16: assignPixels<uint16_t>(value, "value->ushortValue", frame.payload); break;
  case PrimitiveType::Int32: assignPixels<int32_t>(value, "value->intValue", frame.payload); break;
  case PrimitiveType::UInt32: assignPixels<uint32_t>(value, "value->uintValue", frame.payload); break;
  case PrimitiveType::Int64: assignPixels<int64_t>(value, "value->longValue", frame.payload); break;
  case PrimitiveType::UInt64: assignPixels<uint64_t>(value, "value->ulongValue", frame.payload); break;
  case PrimitiveType::Float32: assignPixels<float>(value, "value->floatValue", frame.payload); break;
  case PrimitiveType::Float64: assignPixels<double>(value, "value->doubleValue", frame.payload); break;
  case PrimitiveType::Boolean:
  case PrimitiveType::String: throw std::logic_error("unsupported NTNDArray element type");
  }

  value["codec.name"] = std::string("");
  value["compressedSize"] = static_cast<int64_t>(frame.payload.size());
  value["uncompressedSize"] = static_cast<int64_t>(frame.payload.size());
  value["uniqueId"] = frame.uniqueId;
  assignTimestamp(value["dataTimeStamp"], frame.dataTimestampNs);
  applyTimestamp(value);
  value["alarm.severity"] = epicsSevNone;
  value["alarm.status"] = epicsAlarmNone;
  value["alarm.message"] = std::string("");

  pvxs::shared_array<pvxs::Value> dimensions(frame.shape.size());
  for (size_t index = 0; index < frame.shape.size(); ++index) {
    auto dimension = value["dimension"].allocMember();
    dimension["size"] = frame.shape[index];
    dimension["offset"] = static_cast<int32_t>(0);
    dimension["fullSize"] = frame.shape[index];
    dimension["binning"] = static_cast<int32_t>(1);
    dimension["reverse"] = false;
    dimensions[index] = dimension;
  }
  value["dimension"] = dimensions.freeze();

  pvxs::shared_array<pvxs::Value> attributes(1u);
  auto colorMode = value["attribute"].allocMember();
  colorMode["name"] = std::string("ColorMode");
  colorMode["value"] = static_cast<uint16_t>(frame.colorMode);
  pvxs::shared_array<std::string> tags;
  colorMode["tags"] = tags.freeze();
  colorMode["descriptor"] = std::string("NDColorMode_t");
  colorMode["alarm.severity"] = epicsSevNone;
  colorMode["alarm.status"] = epicsAlarmNone;
  colorMode["alarm.message"] = std::string("");
  assignTimestamp(colorMode["timeStamp"], frame.dataTimestampNs);
  colorMode["sourceType"] = static_cast<int32_t>(0);
  colorMode["source"] = std::string("driver");
  attributes[0] = colorMode;
  value["attribute"] = attributes.freeze();
  return value;
}

}  // namespace

pvxs::Value buildNTNDArrayValue(const NDArrayFrame& frame) {
  return populateNTNDArrayValue(pvxs::nt::NTNDArray{}.create(), frame);
}

pvxs::Value buildNTNDArrayValue(const NDArrayFrame& frame, const pvxs::Value& prototype) {
  return populateNTNDArrayValue(prototype.cloneEmpty(), frame);
}

}  // namespace redis_pvxs_ioc
