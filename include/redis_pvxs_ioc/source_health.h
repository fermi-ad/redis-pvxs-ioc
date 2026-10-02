#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "RedisAdapter.hpp"
#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

using SourceClock = std::chrono::steady_clock;

struct SourceStatus {
  std::string pv, backend, key, role, state, error, inspection;
  bool required = true, valid = false, fresh = false, stale = false, ready = false;
  uint32_t staleAfterMs = 0, readerProbeMs = 0;
  int64_t lastValidAgeMs = -1;
  std::string lastValidCursor;
  uint64_t lastValidEpoch = 0, invalidSamples = 0, staleTransitions = 0, staleCallbacks = 0;
  RedisAdapter::ReaderStatus reader;
};

// Owned by one runtime and protected by its mutex. It describes accepted
// source samples, independently of the value retained for clients.
struct SourceSampleState {
  bool valid = false, wasStale = false;
  std::string cursor, error = "No valid source data";
  uint64_t epoch = 0, invalidSamples = 0, staleTransitions = 0, staleCallbacks = 0;
  uint64_t acceptedReadRejections = 0, seenEpoch = 0;
  SourceClock::time_point received{};
  void accept(const std::string& id, uint64_t capturedEpoch, SourceClock::time_point now = SourceClock::now());
  void reject(const std::string& reason, uint64_t capturedEpoch = 1);
};

SourceStatus assessSource(SourceSampleState& sample, RedisAdapter::ReaderStatus reader,
                         const PVConfig& config, const std::string& name, const RouteConfig& route,
                         const char* role, bool active, uint32_t probeMs, SourceClock::time_point now);
std::string streamKindName(RedisAdapter::StreamKind kind);

}  // namespace redis_pvxs_ioc
