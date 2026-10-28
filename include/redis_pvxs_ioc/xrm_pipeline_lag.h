#pragma once

#include <stdexcept>
#include <string>

#include "RedisAdapter.hpp"

namespace redis_pvxs_ioc {

// Matching endpoint rates/counts cannot prove that intermediate batches used
// the same configuration. A nominal batch-distance estimate is not therefore
// a freshness bound. Only an equal observed tail and processed cursor proves
// that this reader was caught up at the time of its latest Redis observation.
inline bool xrmPipelineCaughtUp(const std::string& processedCursor, const std::string& observedCursor) {
  if (processedCursor.empty() || observedCursor.empty()) return false;
  try {
    return RedisAdapter::compareStreamIds(processedCursor, observedCursor) == 0;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

}  // namespace redis_pvxs_ioc
