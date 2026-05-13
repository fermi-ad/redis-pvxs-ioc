#include <cassert>
#include <ctime>
#include <iostream>
#include <string>

#include <alarm.h>

#include "redis_pvxs_ioc/alarm_publisher.h"

using namespace redis_pvxs_ioc;

namespace {

const std::string& valueAt(const AlarmStreamFields& fields, const std::string& key) {
  for (const auto& field : fields) {
    if (field.first == key) {
      return field.second;
    }
  }
  assert(false && "missing alarm stream field");
  static const std::string empty;
  return empty;
}

}  // namespace

int main() {
  const std::time_t timestamp = 1774477060;

  const auto alarm = makeAlarmStreamFields("DEMO:magnet:current", AlarmState{epicsSevMajor, epicsAlarmHiHi, "High alarm"}, timestamp);
  assert(alarm.size() == 6u);
  assert(alarm[0].first == "device");
  assert(alarm[1].first == "source");
  assert(alarm[2].first == "severity");
  assert(alarm[3].first == "timestamp");
  assert(alarm[4].first == "detail");
  assert(alarm[5].first == "message");
  assert(valueAt(alarm, "device") == "DEMO:magnet:current");
  assert(valueAt(alarm, "source") == "HIHI");
  assert(valueAt(alarm, "severity") == "MAJOR");
  assert(valueAt(alarm, "timestamp") == "1774477060");
  assert(valueAt(alarm, "detail") == "HIHI");
  assert(valueAt(alarm, "message") == "High alarm");

  const auto defaultMessage = makeAlarmStreamFields("DEMO:low", AlarmState{epicsSevMinor, epicsAlarmLow, ""}, timestamp);
  assert(valueAt(defaultMessage, "message") == "LOW alarm is MINOR");

  const auto clear = makeAlarmStreamFields("DEMO:magnet:current", AlarmState{epicsSevNone, epicsAlarmNone, ""}, timestamp);
  assert(clear.size() == 5u);
  assert(valueAt(clear, "source") == "NO_ALARM");
  assert(valueAt(clear, "severity") == "NO_ALARM");
  assert(valueAt(clear, "timestamp") == "1774477060");
  assert(valueAt(clear, "detail") == "NO_ALARM");
  for (const auto& field : clear) {
    assert(field.first != "message");
  }

  std::cout << "alarm publisher tests passed\n";
  return 0;
}
