#include "redis_pvxs_ioc/source_health.h"
#include <algorithm>

namespace redis_pvxs_ioc {

void SourceSampleState::accept(const std::string& id, uint64_t capturedEpoch, SourceClock::time_point now) {
  valid = true; wasStale = false; cursor = id; epoch = seenEpoch = capturedEpoch; received = now; error.clear();
}
void SourceSampleState::reject(const std::string& reason, uint64_t capturedEpoch) {
  valid = false; seenEpoch = capturedEpoch; error = reason; ++invalidSamples;
}

std::string streamKindName(RedisAdapter::StreamKind kind) {
  switch (kind) {
  case RedisAdapter::StreamKind::Missing: return "missing";
  case RedisAdapter::StreamKind::Invalid: return "wrong-type";
  case RedisAdapter::StreamKind::Stream: return "stream";
  default: return "unknown";
  }
}

SourceStatus assessSource(SourceSampleState& sample, RedisAdapter::ReaderStatus reader,
                         const PVConfig& config, const std::string& name, const RouteConfig& route,
                         const char* role, bool active, uint32_t probeMs, SourceClock::time_point now) {
  SourceStatus status;
  status.pv = name; status.backend = route.backend; status.key = route.key; status.role = role;
  status.required = config.sourceHealth.required; status.staleAfterMs = config.sourceHealth.staleAfterMs;
  status.readerProbeMs = probeMs;
  status.reader = std::move(reader);
  status.lastValidCursor = sample.cursor; status.lastValidEpoch = sample.epoch;
  if (sample.received != SourceClock::time_point{})
    status.lastValidAgeMs = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(now - sample.received).count());
  status.valid = sample.valid && sample.epoch == status.reader.epoch;
  status.stale = status.valid && status.staleAfterMs && status.lastValidAgeMs >= status.staleAfterMs;
  status.fresh = status.valid && !status.stale;
  if (status.stale && !sample.wasStale) ++sample.staleTransitions;
  sample.wasStale = status.stale;
  status.invalidSamples = sample.invalidSamples; status.staleTransitions = sample.staleTransitions;
  status.staleCallbacks = sample.staleCallbacks;
  status.inspection = !probeMs ? "disabled" : status.reader.inspected ? "available"
      : status.reader.inspectionRejections ? "rejected" : status.reader.inspectionFailures ? "unavailable" : "pending";
  if (!active || !status.reader.active) status.state = "inactive";
  else if (!status.reader.connected) { status.state = "disconnected"; status.error = "Source reader disconnected"; }
  else if (status.reader.streamKind == RedisAdapter::StreamKind::Missing) {
    status.state = "missing"; status.error = "Source stream missing";
  } else if (status.reader.streamKind == RedisAdapter::StreamKind::Invalid) {
    status.state = "wrong-type"; status.error = "Source key is not a Redis stream";
  } else if (status.reader.readRejections > sample.acceptedReadRejections) {
    status.state = "read-rejected"; status.error = "Source read rejected; waiting for valid data";
  } else if (!sample.valid && sample.seenEpoch == status.reader.epoch) {
    status.state = "invalid"; status.error = sample.error;
  } else if (sample.epoch != status.reader.epoch) {
    status.state = "waiting"; status.error = "Waiting for valid data in current source epoch";
  } else if (!status.valid) { status.state = "invalid"; status.error = sample.error; }
  else if (status.stale) { status.state = "stale"; status.error = "Source freshness deadline exceeded"; }
  else { status.state = "ready"; status.ready = true; }
  return status;
}

}  // namespace redis_pvxs_ioc
