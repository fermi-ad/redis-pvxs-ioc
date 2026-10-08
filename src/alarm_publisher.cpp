#include "redis_pvxs_ioc/alarm_publisher.h"

#include <alarm.h>

#include <ctime>
#include <vector>

namespace redis_pvxs_ioc {

AlarmStreamFields makeAlarmStreamFields(const std::string& pvName, const AlarmState& state, const std::time_t timestamp) {
  const char* const statusText =
      (state.status >= 0 && state.status < ALARM_NSTATUS) ? epicsAlarmConditionStrings[state.status] : "INVALID";
  const char* const severityText =
      (state.severity >= 0 && state.severity < ALARM_NSEV) ? epicsAlarmSeverityStrings[state.severity]
                                                           : epicsAlarmSeverityStrings[epicsSevInvalid];
  const bool clear = state.status == epicsAlarmNone && state.severity == epicsSevNone;
  const std::string status(statusText);
  const std::string severity(severityText);

  AlarmStreamFields fields;
  fields.emplace_back("device", pvName);
  fields.emplace_back("source", status);
  fields.emplace_back("severity", severity);
  fields.emplace_back("timestamp", std::to_string(timestamp));
  fields.emplace_back("detail", status);

  if (!clear) {
    fields.emplace_back("message", state.message.empty() ? status + " alarm is " + severity : state.message);
  }

  return fields;
}

} // namespace redis_pvxs_ioc
