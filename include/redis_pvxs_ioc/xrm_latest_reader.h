#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "RedisAdapter.hpp"
#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct XrmTailMetadata {
  bool present = false;
  std::string session, connection, batch, sampleCount, sampleRate;
};

// XRM has large entries, so never enqueue batches on RedisAdapter's callback
// pool. This reader limits each response and executes its callback on the same
// thread before obtaining another. Redis materializes each requested stream
// entry before Lua can check its size; these limits do not bound Redis itself.
// Redis trims may still cause gaps.
class XrmLatestReader {
public:
  XrmLatestReader(RedisConfig backend, std::string key, uint64_t maxPayloadBytes,
                  uint32_t idlePollMs, RedisAdapter::MetadataStreamCallback callback);
  ~XrmLatestReader();
  XrmLatestReader(const XrmLatestReader&) = delete;
  XrmLatestReader& operator=(const XrmLatestReader&) = delete;

  // Call once before start(). The latest initial value is read with the same
  // response limits as all subsequent entries. If Redis is unavailable or
  // cannot run the script, the worker continues seeking a latest snapshot.
  // A rejected entry advances the cursor and is reflected in readRejections.
  RedisAdapter::StreamSnapshot snapshot();
  void start();
  void reset() noexcept;
  RedisAdapter::ReaderStatus status() const;
  XrmTailMetadata tailMetadata() const;

private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace redis_pvxs_ioc
