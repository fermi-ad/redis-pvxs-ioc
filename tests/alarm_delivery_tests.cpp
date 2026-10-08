#include "redis_pvxs_ioc/alarm_publisher.h"
#include "hiredis.h"
#include <alarm.h>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

template<class F> void eventually(F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(2ms);
  }
}

struct Redis {
  std::unique_ptr<redisContext, decltype(&redisFree)> context;
  explicit Redis(int port) : context(redisConnect("127.0.0.1", port), &redisFree) {
    assert(context && !context->err);
    const timeval timeout{3, 0};
    assert(redisSetTimeout(context.get(), timeout) == REDIS_OK);
  }
  Reply command(std::initializer_list<std::string> args) {
    std::vector<const char*> argv;
    std::vector<size_t> lengths;
    for (const auto& arg : args) { argv.push_back(arg.data()); lengths.push_back(arg.size()); }
    Reply reply(static_cast<redisReply*>(redisCommandArgv(context.get(), argv.size(), argv.data(), lengths.data())), &freeReplyObject);
    assert(reply && reply->type != REDIS_REPLY_ERROR);
    return reply;
  }
  using Fields = std::map<std::string, std::string>;
  std::vector<Fields> rows(const std::string& stream) {
    const auto reply = command({"XRANGE", stream, "-", "+"});
    assert(reply->type == REDIS_REPLY_ARRAY);
    std::vector<Fields> result;
    for (size_t n = 0; n < reply->elements; ++n) {
      const auto fields = reply->element[n]->element[1];
      Fields values;
      for (size_t i = 0; i < fields->elements; i += 2)
        values.emplace(std::string(fields->element[i]->str, fields->element[i]->len),
                       std::string(fields->element[i + 1]->str, fields->element[i + 1]->len));
      result.push_back(std::move(values));
    }
    return result;
  }
};

int main() {
  const auto port = std::atoi(std::getenv("REDIS_PVXS_TEST_REDIS_PORT"));
  assert(port > 0);
  Redis redis(port);
  AlarmPublisherOptions options;
  options.queueEntries = 8; options.stateBytes = 256 * 1024;
  options.ioTimeoutMs = 200; options.retryMs = 30; options.heartbeatMs = 30;
  const AlarmState clear{}, minor{epicsSevMinor, epicsAlarmHigh, "high"};
  const AlarmState major{epicsSevMajor, epicsAlarmHiHi, "very high"};
  const std::string stream = "alarms:delivery-test";
  AlarmPublisher publisher("127.0.0.1", port, stream, {}, {}, options);
  const auto baseline = publisher.status().reservedBytes;
  auto active = publisher.prepare("TEST:one", clear);
  active->activate();
  std::this_thread::sleep_for(60ms);
  assert(publisher.status().state == "staged");
  assert(redis.command({"EXISTS", stream})->integer == 0);
  publisher.activate();
  eventually([&] { return publisher.status().state == "ready"; });
  auto initial = redis.rows(stream);
  assert(initial.size() == 1 && initial[0].at("severity") == "NO_ALARM");
  assert(initial[0].size() == 5 && !initial[0].count("message"));
  assert(std::stoll(initial[0].at("timestamp")) > 0);

  // Healthy transitions remain FIFO, including a return to the initial state.
  active->update(minor); active->update(major); active->update(clear);
  eventually([&] { return publisher.status().sent == 4 && publisher.status().state == "ready"; });
  auto transitions = redis.rows(stream);
  assert(transitions.size() == 4);
  assert(transitions[1].at("severity") == "MINOR" && transitions[1].at("message") == "high");
  assert(transitions[2].at("severity") == "MAJOR" && transitions[2].at("source") == "HIHI");
  assert(transitions[3].at("severity") == "NO_ALARM");

  // Preparing a same-name replacement has no external effect. Old callbacks
  // cannot queue another value once the new registration becomes authoritative.
  auto staged = publisher.prepare("TEST:one", major);
  staged->update(minor);
  const auto before = publisher.status().sent;
  std::this_thread::sleep_for(60ms);
  assert(publisher.status().sent == before);
  staged.reset();
  auto replacement = publisher.prepare("TEST:one", major);
  replacement->activate();
  active->update(minor);
  eventually([&] { return publisher.status().sent == before + 1 && publisher.status().state == "ready"; });
  assert(redis.rows(stream).back().at("severity") == "MAJOR");
  active->retire(); active.reset();
  active = std::move(replacement);

  // Wrong-type Redis rejection is counted separately from transport failure.
  // Restoring the destination reconciles the current state without a new update.
  redis.command({"DEL", stream}); redis.command({"SET", stream, "wrong-type"});
  const auto transportBefore = publisher.status().transportFailures;
  active->update(minor);
  eventually([&] { return publisher.status().rejected > 0; });
  active->update(major); active->update(clear);
  assert(publisher.status().transportFailures == transportBefore);
  redis.command({"DEL", stream});
  eventually([&] { return publisher.status().state == "ready"; });
  auto recovered = redis.rows(stream);
  assert(!recovered.empty() && recovered.back().at("severity") == "NO_ALARM");
  const auto recoveredCount = recovered.size();

  // The heartbeat notices a quiescent disconnect and republishes current state.
  redis.command({"CLIENT", "KILL", "TYPE", "normal", "SKIPME", "yes"});
  eventually([&] { return publisher.status().transportFailures > transportBefore; });
  eventually([&] { return publisher.status().state == "ready"; });
  recovered = redis.rows(stream);
  assert(recovered.size() == recoveredCount + 1 && recovered.back().at("severity") == "NO_ALARM");

  // Hold Redis unavailable while callbacks overrun the queue. Admission remains
  // bounded and fast; recovery sends the final current state, not stale history.
  redis.command({"CLIENT", "PAUSE", "1500", "ALL"});
  const auto started = std::chrono::steady_clock::now();
  for (size_t i = 0; i < 10000; ++i) active->update(i % 2 ? minor : major);
  active->update(clear);
  assert(std::chrono::steady_clock::now() - started < 1s);
  auto pressure = publisher.status();
  assert(pressure.coalescedUpdates && pressure.discardedTransitions);
  assert(pressure.queued <= options.queueEntries && pressure.reservedBytes <= options.stateBytes);
  eventually([&] { return publisher.status().transportFailures > transportBefore + 1; });
  eventually([&] { return publisher.status().state == "ready"; });
  assert(redis.rows(stream).back().at("severity") == "NO_ALARM");

  // Messages have a fixed bound; the end of a UTF-8 code point is not split.
  AlarmState longMessage = major;
  longMessage.message = std::string(508, 'x') + "\xe2\x82\xac" + std::string(20, 'z');
  active->update(longMessage);
  eventually([&] { return publisher.status().state == "ready"; });
  assert(publisher.status().messagesTruncated == 1);
  assert(redis.rows(stream).back().at("message") == std::string(508, 'x') + "...");
  active->retire(); active.reset();
  eventually([&] { return publisher.status().reservedBytes == baseline; });
  publisher.stop();
  assert(publisher.status().state == "stopped");

  // Reservations include staged state, reject overflow without side effects,
  // and become available again only after the prior registration is released.
  options.queueEntries = 1; options.stateBytes = 1024 + 1024 + 2 * std::string("TEST:one").size();
  AlarmPublisher bounded("127.0.0.1", port, "alarms:budget-test", {}, {}, options);
  auto reserved = bounded.prepare("TEST:one", clear);
  try { bounded.prepare("TEST:two", major); assert(false); } catch (const std::runtime_error&) {}
  assert(bounded.status().registrations == 1 && bounded.status().reservedBytes == options.stateBytes);
  assert(redis.command({"EXISTS", "alarms:budget-test"})->integer == 0);
  reserved.reset();
  assert(bounded.status().registrations == 0 && bounded.status().reservedBytes == 1024);
  bounded.prepare("TEST:two", major);
  assert(bounded.status().peakBytes == options.stateBytes);
}
