#include "redis_pvxs_ioc/alarm_publisher.h"

#include <alarm.h>

#include "hiredis.h"

namespace redis_pvxs_ioc {

AlarmPublisher::AlarmPublisher(std::string host, const int port, std::string stream)
    : host_(std::move(host)), port_(port), stream_(std::move(stream)) {}

AlarmPublisher::~AlarmPublisher() {
  resetConnection();
}

void AlarmPublisher::publishTransition(const std::string& pvName, const AlarmState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ensureConnected()) {
    return;
  }

  const char* const statusText =
      (state.status >= 0 && state.status < ALARM_NSTATUS) ? epicsAlarmConditionStrings[state.status] : "INVALID";
  const char* const severityText =
      (state.severity >= 0 && state.severity < ALARM_NSEV) ? epicsAlarmSeverityStrings[state.severity]
                                                           : epicsAlarmSeverityStrings[epicsSevInvalid];

  redisReply* reply = nullptr;
  if (state.status == epicsAlarmNone && state.severity == epicsSevNone) {
    reply = static_cast<redisReply*>(redisCommand(context_,
                                                  "XADD %s MAXLEN 99999 * device %s source %s severity %s",
                                                  stream_.c_str(),
                                                  pvName.c_str(),
                                                  statusText,
                                                  severityText));
  } else {
    reply = static_cast<redisReply*>(redisCommand(context_,
                                                  "XADD %s MAXLEN 99999 * device %s source %s severity %s message %s",
                                                  stream_.c_str(),
                                                  pvName.c_str(),
                                                  statusText,
                                                  severityText,
                                                  state.message.c_str()));
  }

  if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
    resetConnection();
  }

  freeReplyObject(reply);
}

bool AlarmPublisher::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return context_ != nullptr && context_->err == 0;
}

const std::string& AlarmPublisher::stream() const {
  return stream_;
}

bool AlarmPublisher::ensureConnected() {
  if (context_ != nullptr && context_->err == 0) {
    return true;
  }
  resetConnection();
  context_ = redisConnect(host_.c_str(), port_);
  return context_ != nullptr && context_->err == 0;
}

void AlarmPublisher::resetConnection() {
  if (context_ != nullptr) {
    redisFree(context_);
    context_ = nullptr;
  }
}

}  // namespace redis_pvxs_ioc
