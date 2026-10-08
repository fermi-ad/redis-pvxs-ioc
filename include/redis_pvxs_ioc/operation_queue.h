#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace redis_pvxs_ioc {

enum class OperationFailure { Overload, Deadline, Cancelled, Stopped, Exception };

struct OperationQueueLimits {
  size_t workers = 4;
  size_t queuedPerKey = 16;
  size_t residentBytes = 64u * 1024u * 1024u;
};

struct OperationQueueStats {
  uint64_t accepted = 0, completed = 0, overloaded = 0, expired = 0, cancelled = 0;
  size_t queued = 0, running = 0, residentBytes = 0, peakBytes = 0;
};

// One active task per canonical key, including while it is being cancelled.
// Deadlines/replies are independent of worker progress. An active task retains
// its lane and memory reservation until run() returns, so an uncertain write
// cannot be overtaken by the next write on that key.
class OperationQueue {
public:
  using Clock = std::chrono::steady_clock;
  struct Work {
    std::string key;
    size_t bytes = 0;
    Clock::time_point deadline;
    std::function<void()> run;
    std::function<void(OperationFailure, const std::string&)> fail;
  };
  explicit OperationQueue(OperationQueueLimits limits = {});
  ~OperationQueue();
  OperationQueue(const OperationQueue&) = delete;
  OperationQueue& operator=(const OperationQueue&) = delete;

  // Zero means rejected; fail() is called outside the queue lock.
  uint64_t submit(Work work);
  void cancel(uint64_t id);
  void shutdown();
  OperationQueueStats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redis_pvxs_ioc
