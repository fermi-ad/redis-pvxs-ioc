#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include <pvxs/data.h>
#include <pvxs/nt.h>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct AlarmState {
  int severity = 0;
  int status = 0;
  std::string message;
};

pvxs::TypeCode pvxsTypeCodeFor(const PVConfig& config);
pvxs::Value createInitialValue(const PVConfig& config);
void applyStandardMetadata(pvxs::Value& value, const PVConfig& config);
void applyTimestamp(pvxs::Value& value, uint64_t timestampNs = 0);
bool hasAlarmRules(const PVConfig& config);
AlarmState evaluateNumericAlarm(const PVConfig& config, double value, int priorStatus);
void applyAlarmFields(pvxs::Value& value, const PVConfig& config, const AlarmState& alarmState);
std::vector<std::string> standardDisplayFormChoices();
int displayFormIndex(DisplayForm form);
uint64_t currentTimeNs();

template <typename T>
T initialScalarOr(const TypedValue& value, const T& fallback) {
  if (const auto* typed = std::get_if<T>(&value)) {
    return *typed;
  }
  return fallback;
}

template <typename T>
std::vector<T> initialVectorOr(const TypedValue& value, const std::vector<T>& fallback = {}) {
  if (const auto* typed = std::get_if<std::vector<T>>(&value)) {
    return *typed;
  }
  return fallback;
}

template <typename T>
T applyForwardTransform(const PVConfig& config, T raw) {
  if constexpr (std::is_floating_point_v<T>) {
    if (config.transform) {
      return static_cast<T>(raw * static_cast<T>(config.transform->scale) +
                            static_cast<T>(config.transform->offset));
    }
  }
  return raw;
}

template <typename T>
std::vector<T> applyForwardTransform(const PVConfig& config, const std::vector<T>& raw) {
  if constexpr (std::is_floating_point_v<T>) {
    if (config.transform) {
      std::vector<T> transformed(raw);
      for (auto& item : transformed) {
        item = applyForwardTransform(config, item);
      }
      return transformed;
    }
  }
  return raw;
}

template <typename T>
T applyInverseTransform(const PVConfig& config, T present) {
  if constexpr (std::is_floating_point_v<T>) {
    if (config.transform) {
      return static_cast<T>((present - static_cast<T>(config.transform->offset)) /
                            static_cast<T>(config.transform->scale));
    }
  }
  return present;
}

template <typename T>
std::vector<T> applyInverseTransform(const PVConfig& config, const std::vector<T>& present) {
  if constexpr (std::is_floating_point_v<T>) {
    if (config.transform) {
      std::vector<T> transformed(present);
      for (auto& item : transformed) {
        item = applyInverseTransform(config, item);
      }
      return transformed;
    }
  }
  return present;
}

template <typename T>
void assignScalarValue(pvxs::Value& value, const T& input) {
  value["value"] = input;
}

template <typename T>
void assignArrayValue(pvxs::Value& value, const std::vector<T>& input) {
  pvxs::shared_array<T> array(input.begin(), input.end());
  value["value"] = array.freeze();
}

template <typename T>
T scalarValueFrom(const pvxs::Value& value) {
  return value["value"].as<T>();
}

template <typename T>
std::vector<T> arrayValueFrom(const pvxs::Value& value) {
  const auto array = value["value"].as<pvxs::shared_array<const T>>();
  return std::vector<T>(array.begin(), array.end());
}

template <typename T>
bool valuesEqual(const T& lhs, const T& rhs) {
  if constexpr (std::is_floating_point_v<T>) {
    const auto scale = std::max(std::fabs(lhs), std::fabs(rhs));
    const auto tolerance = std::numeric_limits<T>::epsilon() * std::max<T>(T{1}, scale) * T{8};
    return std::fabs(lhs - rhs) <= tolerance;
  } else {
    return lhs == rhs;
  }
}

template <typename T>
bool valuesEqual(const std::vector<T>& lhs, const std::vector<T>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t index = 0; index < lhs.size(); ++index) {
    if (!valuesEqual(lhs[index], rhs[index])) {
      return false;
    }
  }
  return true;
}

}  // namespace redis_pvxs_ioc
