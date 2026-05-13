#include "redis_pvxs_ioc/alarm_publisher.h"

#include <alarm.h>

#include <ctime>
#include <vector>

#include "hiredis.h"

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

AlarmPublisher::AlarmPublisher(std::string host,
                               const int port,
                               std::string stream,
                               std::string user,
                               std::string password)
    : host_(std::move(host)),
      port_(port),
      stream_(std::move(stream)),
      user_(std::move(user)),
      password_(std::move(password)) {}

AlarmPublisher::~AlarmPublisher() {
  resetConnection();
}

void AlarmPublisher::publishTransition(const std::string& pvName, const AlarmState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ensureConnected()) {
    return;
  }

  const auto fields = makeAlarmStreamFields(pvName, state, std::time(nullptr));
  std::vector<std::string> args{"XADD", stream_, "MAXLEN", "99999", "*"};
  args.reserve(5 + fields.size() * 2);
  for (const auto& field : fields) {
    args.push_back(field.first);
    args.push_back(field.second);
  }

  std::vector<const char*> argv;
  std::vector<size_t> argvlen;
  argv.reserve(args.size());
  argvlen.reserve(args.size());
  for (const auto& arg : args) {
    argv.push_back(arg.data());
    argvlen.push_back(arg.size());
  }

  redisReply* reply = static_cast<redisReply*>(
      redisCommandArgv(context_, static_cast<int>(argv.size()), argv.data(), argvlen.data()));

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
  if (context_ == nullptr || context_->err != 0) {
    resetConnection();
    return false;
  }

  if (!user_.empty() || !password_.empty()) {
    redisReply* reply = nullptr;
    if (!user_.empty()) {
      reply = static_cast<redisReply*>(redisCommand(context_, "AUTH %s %s", user_.c_str(), password_.c_str()));
    } else {
      reply = static_cast<redisReply*>(redisCommand(context_, "AUTH %s", password_.c_str()));
    }

    const bool ok = reply != nullptr && reply->type != REDIS_REPLY_ERROR;
    freeReplyObject(reply);
    if (!ok) {
      resetConnection();
      return false;
    }
  }

  return true;
}

void AlarmPublisher::resetConnection() {
  if (context_ != nullptr) {
    redisFree(context_);
    context_ = nullptr;
  }
}

}  // namespace redis_pvxs_ioc
