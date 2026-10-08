#include "redis_pvxs_ioc/operation_queue.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redis_pvxs_ioc {

struct OperationQueue::Impl {
  struct Task;
  using Tasks = std::list<std::shared_ptr<Task>>;
  using Deadlines = std::multimap<Clock::time_point, uint64_t>;
  struct Lane { bool active = false, ready = false; Tasks queue; };
  struct Task {
    uint64_t id;
    Work work;
    size_t charge;
    bool active = false, failed = false;
    Tasks::iterator position;
    Deadlines::iterator expiry;
  };

  explicit Impl(OperationQueueLimits settings) : limits(settings) {
    if (!limits.workers || !limits.queuedPerKey || limits.residentBytes < 256)
      throw std::invalid_argument("operation queue limits must be positive");
    try {
      for (size_t i = 0; i < limits.workers; ++i) workers.emplace_back([this] { worker(); });
      timer = std::thread([this] { expire(); });
    } catch (...) {
      stop();
      throw;
    }
  }

  static void reject(const std::shared_ptr<Task>& task, OperationFailure reason, const std::string& text) {
    try { task->work.fail(reason, text); } catch (...) { /* never lose a worker to client teardown */ }
  }

  void ready(const std::string& key, Lane& lane) {
    if (!lane.active && !lane.ready && !lane.queue.empty()) {
      runnable.push_back(key);
      lane.ready = true;
      changed.notify_all();
    }
  }

  void releaseQueued(const std::shared_ptr<Task>& task) {
    auto& lane = lanes.at(task->work.key);
    lane.queue.erase(task->position);
    --counts.queued;
    counts.residentBytes -= task->charge;
    tasks.erase(task->id);
    // A runnable token may still refer to this lane; the worker consumes it.
    if (!lane.active && !lane.ready && lane.queue.empty()) lanes.erase(task->work.key);
  }

  void worker() {
    for (;;) {
      std::shared_ptr<Task> task;
      bool pastDeadline = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stopped || !runnable.empty(); });
        if (stopped) return;
        const auto key = std::move(runnable.front());
        runnable.pop_front();
        auto found = lanes.find(key);
        if (found == lanes.end()) continue;
        auto& lane = found->second;
        lane.ready = false;
        if (lane.queue.empty()) {
          if (!lane.active) lanes.erase(found);
          continue;
        }
        task = lane.queue.front();
        lane.queue.pop_front();
        task->active = lane.active = true;
        --counts.queued;
        ++counts.running;
        if (task->work.deadline <= Clock::now()) {
          task->failed = pastDeadline = true;
          ++counts.expired;
        }
      }
      try {
        if (pastDeadline) reject(task, OperationFailure::Deadline, "total operation deadline exceeded before dispatch");
        else task->work.run();
      }
      catch (const std::exception& ex) { reject(task, OperationFailure::Exception, ex.what()); }
      catch (...) { reject(task, OperationFailure::Exception, "operation failed"); }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (task->expiry != deadlines.end()) deadlines.erase(task->expiry);
        tasks.erase(task->id);
        --counts.running;
        counts.residentBytes -= task->charge;
        ++counts.completed;
        auto& lane = lanes.at(task->work.key);
        lane.active = false;
        if (lane.queue.empty()) lanes.erase(task->work.key);
        else ready(task->work.key, lane);
      }
      // Destroy captures without holding the queue lock.
    }
  }

  void expire() {
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopped) {
      if (deadlines.empty()) { changed.wait(lock); continue; }
      const auto deadline = deadlines.begin()->first;
      if (Clock::now() < deadline) { changed.wait_until(lock, deadline); continue; }
      const auto entry = deadlines.begin();
      const auto task = tasks.at(entry->second);
      if (task->failed) {
        // Keep active tasks indexed for shutdown, but no longer timed.
        deadlines.erase(entry);
        task->expiry = deadlines.end();
        continue;
      }
      task->failed = true;
      ++counts.expired;
      deadlines.erase(entry);
      task->expiry = deadlines.end();
      if (!task->active) releaseQueued(task);
      lock.unlock();
      reject(task, OperationFailure::Deadline, "total operation deadline exceeded; not retried");
      lock.lock();
    }
  }

  void stop() {
    std::lock_guard<std::mutex> shutdownGuard(shutdownMutex);
    std::vector<std::shared_ptr<Task>> cancelled;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!stopped) {
        stopped = true;
        for (auto& entry : tasks) {
          if (!entry.second->failed) {
            entry.second->failed = true;
            cancelled.push_back(entry.second);
          }
        }
        changed.notify_all();
      }
    }
    for (const auto& task : cancelled) reject(task, OperationFailure::Stopped, "operation queue stopped");
    for (auto& thread : workers) if (thread.joinable()) thread.join();
    if (timer.joinable()) timer.join();
    decltype(tasks) remainingTasks;
    decltype(lanes) remainingLanes;
    {
      std::lock_guard<std::mutex> lock(mutex);
      remainingTasks.swap(tasks);
      remainingLanes.swap(lanes);
      deadlines.clear(); runnable.clear();
      counts.queued = counts.running = counts.residentBytes = 0;
    }
  }

  OperationQueueLimits limits;
  mutable std::mutex mutex;
  std::mutex shutdownMutex;
  std::condition_variable changed;
  bool stopped = false;
  uint64_t nextId = 0;
  OperationQueueStats counts;
  std::unordered_map<std::string, Lane> lanes;
  std::unordered_map<uint64_t, std::shared_ptr<Task>> tasks;
  Deadlines deadlines;
  std::deque<std::string> runnable;
  std::vector<std::thread> workers;
  std::thread timer;
};

OperationQueue::OperationQueue(OperationQueueLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
OperationQueue::~OperationQueue() { shutdown(); }

uint64_t OperationQueue::submit(Work work) {
  auto task = std::make_shared<Impl::Task>();
  task->work = std::move(work);
  task->charge = std::max<size_t>(task->work.bytes, 256); // empty arrays still consume bookkeeping space
  OperationFailure failure = OperationFailure::Overload;
  std::string message = "operation queue overloaded";
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stopped) {
      failure = OperationFailure::Stopped;
      message = "operation queue stopped";
    } else if (task->work.deadline <= Clock::now()) {
      ++impl_->counts.expired;
      failure = OperationFailure::Deadline;
      message = "total operation deadline exceeded before dispatch";
    } else {
      auto found = impl_->lanes.find(task->work.key);
      const auto queued = found == impl_->lanes.end() ? 0 : found->second.queue.size();
      if (queued < impl_->limits.queuedPerKey && task->charge <= impl_->limits.residentBytes - impl_->counts.residentBytes) {
        task->id = ++impl_->nextId;
        auto& lane = impl_->lanes[task->work.key];
        bool queuedTask = false, timedTask = false;
        try {
          lane.queue.push_back(task);
          task->position = std::prev(lane.queue.end());
          queuedTask = true;
          task->expiry = impl_->deadlines.emplace(task->work.deadline, task->id);
          timedTask = true;
          impl_->tasks.emplace(task->id, task);
          impl_->ready(task->work.key, lane);
        } catch (...) {
          impl_->tasks.erase(task->id);
          if (timedTask) impl_->deadlines.erase(task->expiry);
          if (queuedTask) lane.queue.erase(task->position);
          if (!lane.active && !lane.ready && lane.queue.empty()) impl_->lanes.erase(task->work.key);
          throw;
        }
        ++impl_->counts.accepted;
        ++impl_->counts.queued;
        impl_->counts.residentBytes += task->charge;
        impl_->counts.peakBytes = std::max(impl_->counts.peakBytes, impl_->counts.residentBytes);
        impl_->changed.notify_all();
        return task->id;
      }
      ++impl_->counts.overloaded;
    }
  }
  Impl::reject(task, failure, message);
  return 0;
}

void OperationQueue::cancel(uint64_t id) {
  std::shared_ptr<Impl::Task> task;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->tasks.find(id);
    if (found == impl_->tasks.end() || found->second->failed) return;
    task = found->second;
    task->failed = true;
    ++impl_->counts.cancelled;
    impl_->deadlines.erase(task->expiry);
    task->expiry = impl_->deadlines.end();
    if (!task->active) impl_->releaseQueued(task);
    impl_->changed.notify_all();
  }
  Impl::reject(task, OperationFailure::Cancelled, "operation cancelled; not retried");
}

void OperationQueue::shutdown() { if (impl_) impl_->stop(); }
OperationQueueStats OperationQueue::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->counts;
}

} // namespace redis_pvxs_ioc
