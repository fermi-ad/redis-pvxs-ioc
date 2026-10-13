#pragma once

#include <pvxs/data.h>

namespace redis_pvxs_ioc {

// A monitor carries cached values for unmarked fields as well as its delta.
// Only payload/acquisition-time marks identify a sample publication. A repeated
// marked payload is still a sample and must retain duplicate accounting.
inline bool monitorPublishesSample(const pvxs::Value& value) {
  return value["value"].isMarked(true, true) || value["timeStamp"].isMarked(true, true);
}

}  // namespace redis_pvxs_ioc
