#include "redis_pvxs_ioc/ntndarray.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <alarm.h>

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

template <typename T>
T parseSigned(const std::string& value, const char* name) {
  static_assert(std::is_signed_v<T>);
  T result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    throw std::runtime_error(std::string("invalid signed integer in '") + name + "'");
  }
  return result;
}

PrimitiveType parseDataType(const std::string& text) {
  switch (parseUnsigned<uint32_t>(text, "data_type")) {
  case 0: return PrimitiveType::Int8;
  case 1: return PrimitiveType::UInt8;
  case 2: return PrimitiveType::Int16;
  case 3: return PrimitiveType::UInt16;
  case 4: return PrimitiveType::Int32;
  case 5: return PrimitiveType::UInt32;
  case 6: return PrimitiveType::Int64;
  case 7: return PrimitiveType::UInt64;
  case 8: return PrimitiveType::Float32;
  case 9: return PrimitiveType::Float64;
  default: throw std::runtime_error("unsupported numeric data_type '" + text + "'");
  }
}

NDColorMode parseColorMode(const std::string& text) {
  if (text == "mono") return NDColorMode::Mono;
  if (text == "rgb1") return NDColorMode::RGB1;
  if (text == "rgb2") return NDColorMode::RGB2;
  if (text == "rgb3") return NDColorMode::RGB3;
  throw std::runtime_error("unsupported color_mode '" + text + "'");
}

void skipWhitespace(const std::string_view text, size_t& position) {
  while (position < text.size() &&
         (text[position] == ' ' || text[position] == '\t' ||
          text[position] == '\r' || text[position] == '\n')) {
    ++position;
  }
}

std::vector<int32_t> parseShape(const std::string& value) {
  const std::string_view text(value);
  size_t position = 0;
  std::vector<int32_t> shape;
  skipWhitespace(text, position);
  if (position == text.size() || text[position++] != '[') {
    throw std::runtime_error("shape must be a JSON array");
  }
  for (;;) {
    skipWhitespace(text, position);
    if (position < text.size() && text[position] == ']') {
      ++position;
      break;
    }
    if (shape.size() == 3u) {
      throw std::runtime_error("shape must contain one to three dimensions");
    }
    const size_t first = position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      ++position;
    }
    if (first == position) {
      throw std::runtime_error("shape dimensions must be positive integers");
    }
    const auto size = parseUnsigned<uint64_t>(
        std::string(text.substr(first, position - first)), "shape dimension");
    if (size == 0u ||
        size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      throw std::runtime_error("shape dimension is zero or exceeds int32");
    }
    shape.push_back(static_cast<int32_t>(size));

    skipWhitespace(text, position);
    if (position < text.size() && text[position] == ',') {
      ++position;
      continue;
    }
    if (position < text.size() && text[position] == ']') {
      ++position;
      break;
    }
    throw std::runtime_error("invalid shape JSON");
  }
  skipWhitespace(text, position);
  if (position != text.size()) throw std::runtime_error("invalid shape JSON");
  if (shape.empty()) {
    throw std::runtime_error("shape must contain one to three dimensions");
  }
  return shape;
}

NDTimeSource parseTimeSource(const std::string& value) {
  if (value == "unknown") return NDTimeSource::Unknown;
  if (value == "host") return NDTimeSource::Host;
  if (value == "camera_correlated") return NDTimeSource::CameraCorrelated;
  if (value == "ptp") return NDTimeSource::PTP;
  if (value == "external_trigger") return NDTimeSource::ExternalTrigger;
  throw std::runtime_error("unsupported time_source '" + value + "'");
}

template <typename T>
std::optional<T> parseOptionalUnsigned(const NDArrayAttrs& attrs,
                                       const char* name) {
  const auto it = attrs.find(name);
  if (it == attrs.end()) return std::nullopt;
  return parseUnsigned<T>(it->second, name);
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

NDArrayFrame parseNDArrayFrame(const NDArrayAttrs& attrs,
                               const int64_t streamTimestampNs,
                               const uint64_t maxFrameBytes) {
  if (required(attrs, "schema") != kNTNDArrayRedisSchema) {
    throw std::runtime_error("unsupported schema");
  }
  if (streamTimestampNs <= 0) {
    throw std::runtime_error("Redis Stream ID is outside the supported POSIX range");
  }

  NDArrayFrame frame;
  frame.dataType = parseDataType(required(attrs, "data_type"));
  frame.shape = parseShape(required(attrs, "shape"));
  frame.colorMode = parseColorMode(required(attrs, "color_mode"));
  validateColorShape(frame.colorMode, frame.shape);
  const auto uniqueId = parseSigned<int64_t>(required(attrs, "unique_id"), "unique_id");
  if (uniqueId < std::numeric_limits<int32_t>::min() ||
      uniqueId > std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error("unique_id exceeds NTNDArray int32 range");
  }
  frame.uniqueId = static_cast<int32_t>(uniqueId);
  frame.dataTimestampNs = static_cast<uint64_t>(streamTimestampNs);
  const auto timeSource = attrs.find("time_source");
  if (timeSource != attrs.end()) {
    frame.provenance.source = parseTimeSource(timeSource->second);
  }
  frame.provenance.uncertaintyNs =
      parseOptionalUnsigned<uint64_t>(attrs, "time_uncertainty_ns");
  frame.provenance.cameraTimestamp =
      parseOptionalUnsigned<uint64_t>(attrs, "camera_timestamp");
  frame.provenance.cameraTimestampHz =
      parseOptionalUnsigned<uint64_t>(attrs, "camera_timestamp_hz");

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
  const auto& payload = required(attrs, kNTNDArrayPayloadField);
  if (payload.size() != expectedBytes) {
    throw std::runtime_error("payload length does not match data_type and shape");
  }
  frame.payload.assign(payload.begin(), payload.end());

  // fermi-ad/redis-adapter/ntndarray:1 fixes multi-byte pixels to little
  // endian, so no per-frame byte-order field is needed.
  if (width > 1u && !hostIsLittleEndian()) {
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

  size_t attributeCount = 3u;
  if (frame.provenance.uncertaintyNs) ++attributeCount;
  if (frame.provenance.cameraTimestamp) ++attributeCount;
  if (frame.provenance.cameraTimestampHz) ++attributeCount;
  pvxs::shared_array<pvxs::Value> attributes(attributeCount);
  size_t attributeIndex = 0u;
  const auto addAttribute = [&](const std::string& name,
                                const std::string& descriptor,
                                const auto attributeValue,
                                const std::string& source) {
    auto attribute = value["attribute"].allocMember();
    attribute["name"] = name;
    attribute["value"] = attributeValue;
    pvxs::shared_array<std::string> tags;
    attribute["tags"] = tags.freeze();
    attribute["descriptor"] = descriptor;
    attribute["alarm.severity"] = epicsSevNone;
    attribute["alarm.status"] = epicsAlarmNone;
    attribute["alarm.message"] = std::string("");
    assignTimestamp(attribute["timeStamp"], frame.dataTimestampNs);
    attribute["sourceType"] = static_cast<int32_t>(0);
    attribute["source"] = source;
    attributes[attributeIndex++] = attribute;
  };
  addAttribute("ColorMode", "NDColorMode_t",
               static_cast<uint16_t>(frame.colorMode), "driver");
  addAttribute("AcquisitionTimeNs", "POSIX nanoseconds",
               frame.dataTimestampNs, "Redis Stream ID");
  addAttribute("AcquisitionTimeSource", "NDTimeSource",
               static_cast<uint32_t>(frame.provenance.source), "envelope");
  if (frame.provenance.uncertaintyNs) {
    addAttribute("AcquisitionTimeUncertaintyNs", "nanoseconds",
                 *frame.provenance.uncertaintyNs, "envelope");
  }
  if (frame.provenance.cameraTimestamp) {
    addAttribute("CameraTimestamp", "camera clock ticks",
                 *frame.provenance.cameraTimestamp, "envelope");
  }
  if (frame.provenance.cameraTimestampHz) {
    addAttribute("CameraTimestampFrequency", "ticks per second",
                 *frame.provenance.cameraTimestampHz, "envelope");
  }
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
