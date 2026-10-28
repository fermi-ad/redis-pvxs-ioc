#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "RedisAdapter.hpp"
#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

// Reads only fixed, length-checked fields of the versioned latest-value hash.
// It does not inspect the associated Redis stream or replay missed entries.
class XrmHashReader {
public:
  XrmHashReader(RedisConfig backend, std::string key, uint64_t maxPayloadBytes,
                uint32_t idlePollMs, RedisAdapter::MetadataStreamCallback callback);
  ~XrmHashReader();
  XrmHashReader(const XrmHashReader&) = delete;
  XrmHashReader& operator=(const XrmHashReader&) = delete;

  RedisAdapter::StreamSnapshot snapshot();
  void start();
  void reset() noexcept;
  RedisAdapter::ReaderStatus status() const;

  // The previous successful poll's request-start time is a conservative lower
  // bound on the publication time of the current changed ID. Empty means no
  // fresh publication has been witnessed since initialization or an outage.
  std::chrono::steady_clock::time_point freshnessProof() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace redis_pvxs_ioc
