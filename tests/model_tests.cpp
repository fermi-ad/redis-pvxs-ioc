#include <cassert>
#include <cmath>
#include <iostream>

#include <alarm.h>

#include "redis_pvxs_ioc/config.h"
#include "redis_pvxs_ioc/util.h"

using namespace redis_pvxs_ioc;

int main() {
  PVConfig pv;
  pv.name = "test";
  pv.type = PrimitiveType::Float64;
  pv.shape = Shape::Scalar;
  pv.read.key = "rb";
  pv.metadata.description = "test pv";
  pv.metadata.units = "A";
  pv.metadata.precision = 3;
  pv.metadata.form = DisplayForm::Engineering;
  pv.metadata.display.low = 0.0;
  pv.metadata.display.high = 10.0;
  pv.metadata.control.low = 0.0;
  pv.metadata.control.high = 12.0;
  pv.alarms.lowWarning = 3.0;
  pv.alarms.highWarning = 7.0;
  pv.transform = LinearTransformConfig{2.0, 1.0};

  auto value = createInitialValue(pv);
  value["value"] = 5.0;

  assert(value["display.description"].as<std::string>() == "test pv");
  assert(value["display.units"].as<std::string>() == "A");
  assert(value["display.precision"].as<int32_t>() == 3);
  assert(value["display.form.index"].as<int32_t>() == 6);
  assert(value["valueAlarm.active"].as<bool>());

  const auto transformed = applyForwardTransform(pv, 2.0);
  const auto raw = applyInverseTransform(pv, transformed);
  assert(std::fabs(transformed - 5.0) < 1e-12);
  assert(std::fabs(raw - 2.0) < 1e-12);

  const auto warning = evaluateNumericAlarm(pv, 8.0, epicsAlarmNone);
  assert(warning.status == epicsAlarmHigh);
  assert(warning.severity == epicsSevMinor);

  pv.alarms.hysteresis = 0.5;
  const auto heldByHysteresis = evaluateNumericAlarm(pv, 6.6, warning.status);
  assert(heldByHysteresis.status == epicsAlarmHigh);
  assert(heldByHysteresis.severity == epicsSevMinor);

  const auto clear = evaluateNumericAlarm(pv, 6.4, warning.status);
  assert(clear.status == epicsAlarmNone);
  assert(clear.severity == epicsSevNone);

  std::cout << "model tests passed\n";
  return 0;
}
