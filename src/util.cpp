#include "redis_pvxs_ioc/util.h"

#include <algorithm>
#include <limits>

#include <alarm.h>
#include <epicsTime.h>

namespace redis_pvxs_ioc {
namespace {

bool isArray(const PVConfig& config) {
  return config.shape == Shape::Array;
}

double nanValue() {
  return std::numeric_limits<double>::quiet_NaN();
}

void assignStringArray(pvxs::Value value, const std::vector<std::string>& input) {
  pvxs::shared_array<std::string> array(input.begin(), input.end());
  value = array.freeze();
}

void assignTypedNumeric(pvxs::Value field, const std::optional<double> input, const double defaultValue = 0.0) {
  const auto scalar = input.value_or(defaultValue);
  switch (field.type().code) {
  case pvxs::TypeCode::Int8: field = static_cast<int8_t>(scalar); break;
  case pvxs::TypeCode::UInt8: field = static_cast<uint8_t>(scalar); break;
  case pvxs::TypeCode::Int16: field = static_cast<int16_t>(scalar); break;
  case pvxs::TypeCode::UInt16: field = static_cast<uint16_t>(scalar); break;
  case pvxs::TypeCode::Int32: field = static_cast<int32_t>(scalar); break;
  case pvxs::TypeCode::UInt32: field = static_cast<uint32_t>(scalar); break;
  case pvxs::TypeCode::Int64: field = static_cast<int64_t>(scalar); break;
  case pvxs::TypeCode::UInt64: field = static_cast<uint64_t>(scalar); break;
  case pvxs::TypeCode::Float32: field = static_cast<float>(scalar); break;
  case pvxs::TypeCode::Float64: field = scalar; break;
  default: break;
  }
}

}  // namespace

pvxs::TypeCode pvxsTypeCodeFor(const PVConfig& config) {
  switch (config.type) {
  case PrimitiveType::Boolean: return isArray(config) ? pvxs::TypeCode::BoolA : pvxs::TypeCode::Bool;
  case PrimitiveType::Int8: return isArray(config) ? pvxs::TypeCode::Int8A : pvxs::TypeCode::Int8;
  case PrimitiveType::UInt8: return isArray(config) ? pvxs::TypeCode::UInt8A : pvxs::TypeCode::UInt8;
  case PrimitiveType::Int16: return isArray(config) ? pvxs::TypeCode::Int16A : pvxs::TypeCode::Int16;
  case PrimitiveType::UInt16: return isArray(config) ? pvxs::TypeCode::UInt16A : pvxs::TypeCode::UInt16;
  case PrimitiveType::Int32: return isArray(config) ? pvxs::TypeCode::Int32A : pvxs::TypeCode::Int32;
  case PrimitiveType::UInt32: return isArray(config) ? pvxs::TypeCode::UInt32A : pvxs::TypeCode::UInt32;
  case PrimitiveType::Int64: return isArray(config) ? pvxs::TypeCode::Int64A : pvxs::TypeCode::Int64;
  case PrimitiveType::UInt64: return isArray(config) ? pvxs::TypeCode::UInt64A : pvxs::TypeCode::UInt64;
  case PrimitiveType::Float32: return isArray(config) ? pvxs::TypeCode::Float32A : pvxs::TypeCode::Float32;
  case PrimitiveType::Float64: return isArray(config) ? pvxs::TypeCode::Float64A : pvxs::TypeCode::Float64;
  case PrimitiveType::String: return pvxs::TypeCode::String;
  }
  return pvxs::TypeCode::Float64;
}

pvxs::Value createInitialValue(const PVConfig& config) {
  const auto typeCode = pvxsTypeCodeFor(config);
  const auto numeric = isNumericType(config.type);
  auto value = pvxs::nt::NTScalar{typeCode, true, numeric, numeric, numeric}.create();
  applyStandardMetadata(value, config);
  applyAlarmFields(value, config, AlarmState{epicsSevNone, epicsAlarmNone, ""});
  applyTimestamp(value);
  return value;
}

void applyStandardMetadata(pvxs::Value& value, const PVConfig& config) {
  if (value["display.description"].valid()) {
    value["display.description"] = config.metadata.description;
  }
  if (value["display.units"].valid()) {
    value["display.units"] = config.metadata.units;
  }
  if (value["display.limitLow"].valid()) {
    assignTypedNumeric(value["display.limitLow"], config.metadata.display.low, 0.0);
  }
  if (value["display.limitHigh"].valid()) {
    assignTypedNumeric(value["display.limitHigh"], config.metadata.display.high, 0.0);
  }
  if (value["display.precision"].valid()) {
    value["display.precision"] = config.metadata.precision.value_or(0);
  }
  if (value["display.form.index"].valid()) {
    value["display.form.index"] = displayFormIndex(config.metadata.form.value_or(DisplayForm::Default));
  }
  if (value["display.form.choices"].valid()) {
    assignStringArray(value["display.form.choices"], standardDisplayFormChoices());
  }
  if (value["control.limitLow"].valid()) {
    assignTypedNumeric(value["control.limitLow"], config.metadata.control.low, 0.0);
  }
  if (value["control.limitHigh"].valid()) {
    assignTypedNumeric(value["control.limitHigh"], config.metadata.control.high, 0.0);
  }
  if (value["control.minStep"].valid()) {
    assignTypedNumeric(value["control.minStep"], config.metadata.minStep, 0.0);
  }
}

void applyTimestamp(pvxs::Value& value, const uint64_t timestampNs) {
  epicsTimeStamp current{};
  uint64_t timestamp = timestampNs;
  if (timestamp == 0 && epicsTimeGetCurrent(&current) == epicsTimeOK) {
    timestamp = static_cast<uint64_t>(current.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH) * 1'000'000'000ULL +
                static_cast<uint64_t>(current.nsec);
  }

  value["timeStamp.secondsPastEpoch"] = static_cast<int64_t>(timestamp / 1'000'000'000ULL);
  value["timeStamp.nanoseconds"] = static_cast<int32_t>(timestamp % 1'000'000'000ULL);
  value["timeStamp.userTag"] = 0;
}

bool hasAlarmRules(const PVConfig& config) {
  return config.alarms.lowAlarm || config.alarms.lowWarning || config.alarms.highWarning || config.alarms.highAlarm;
}

AlarmState evaluateNumericAlarm(const PVConfig& config, const double value, const int priorStatus) {
  AlarmState state{epicsSevNone, epicsAlarmNone, ""};
  const auto& alarms = config.alarms;
  const double hyst = alarms.hysteresis;

  if (!hasAlarmRules(config)) {
    return state;
  }

  if (alarms.highAlarm && priorStatus == epicsAlarmHiHi && value >= *alarms.highAlarm - hyst) {
    return {epicsSevMajor, epicsAlarmHiHi, "High alarm"};
  }
  if (alarms.highWarning && priorStatus == epicsAlarmHigh && value >= *alarms.highWarning - hyst) {
    return {epicsSevMinor, epicsAlarmHigh, "High warning"};
  }
  if (alarms.lowAlarm && priorStatus == epicsAlarmLoLo && value <= *alarms.lowAlarm + hyst) {
    return {epicsSevMajor, epicsAlarmLoLo, "Low alarm"};
  }
  if (alarms.lowWarning && priorStatus == epicsAlarmLow && value <= *alarms.lowWarning + hyst) {
    return {epicsSevMinor, epicsAlarmLow, "Low warning"};
  }

  if (alarms.highAlarm && value >= *alarms.highAlarm) {
    return {epicsSevMajor, epicsAlarmHiHi, "High alarm"};
  }
  if (alarms.lowAlarm && value <= *alarms.lowAlarm) {
    return {epicsSevMajor, epicsAlarmLoLo, "Low alarm"};
  }
  if (alarms.highWarning && value >= *alarms.highWarning) {
    return {epicsSevMinor, epicsAlarmHigh, "High warning"};
  }
  if (alarms.lowWarning && value <= *alarms.lowWarning) {
    return {epicsSevMinor, epicsAlarmLow, "Low warning"};
  }

  return state;
}

void applyAlarmFields(pvxs::Value& value, const PVConfig& config, const AlarmState& alarmState) {
  value["alarm.severity"] = alarmState.severity;
  value["alarm.status"] = alarmState.status;
  value["alarm.message"] = alarmState.message;

  if (!value["valueAlarm.active"].valid()) {
    return;
  }

  const auto enabled = hasAlarmRules(config);
  value["valueAlarm.active"] = enabled;
  const auto floatingDefault =
      (value["valueAlarm.lowAlarmLimit"].type().code == pvxs::TypeCode::Float32 ||
       value["valueAlarm.lowAlarmLimit"].type().code == pvxs::TypeCode::Float64)
          ? nanValue()
          : 0.0;
  assignTypedNumeric(value["valueAlarm.lowAlarmLimit"], config.alarms.lowAlarm, floatingDefault);
  assignTypedNumeric(value["valueAlarm.lowWarningLimit"], config.alarms.lowWarning, floatingDefault);
  assignTypedNumeric(value["valueAlarm.highWarningLimit"], config.alarms.highWarning, floatingDefault);
  assignTypedNumeric(value["valueAlarm.highAlarmLimit"], config.alarms.highAlarm, floatingDefault);
  value["valueAlarm.lowAlarmSeverity"] = config.alarms.lowAlarm ? epicsSevMajor : epicsSevNone;
  value["valueAlarm.lowWarningSeverity"] = config.alarms.lowWarning ? epicsSevMinor : epicsSevNone;
  value["valueAlarm.highWarningSeverity"] = config.alarms.highWarning ? epicsSevMinor : epicsSevNone;
  value["valueAlarm.highAlarmSeverity"] = config.alarms.highAlarm ? epicsSevMajor : epicsSevNone;
  value["valueAlarm.hysteresis"] = config.alarms.hysteresis;
}

std::vector<std::string> standardDisplayFormChoices() {
  return {"Default", "String", "Binary", "Decimal", "Hex", "Exponential", "Engineering"};
}

int displayFormIndex(const DisplayForm form) {
  switch (form) {
  case DisplayForm::Default: return 0;
  case DisplayForm::String: return 1;
  case DisplayForm::Binary: return 2;
  case DisplayForm::Decimal: return 3;
  case DisplayForm::Hex: return 4;
  case DisplayForm::Exponential: return 5;
  case DisplayForm::Engineering: return 6;
  }
  return 0;
}

uint64_t currentTimeNs() {
  epicsTimeStamp current{};
  if (epicsTimeGetCurrent(&current) != epicsTimeOK) {
    return 0;
  }
  return static_cast<uint64_t>(current.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH) * 1'000'000'000ULL +
         static_cast<uint64_t>(current.nsec);
}

}  // namespace redis_pvxs_ioc
