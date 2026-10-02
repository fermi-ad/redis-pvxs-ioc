#include "redis_pvxs_ioc/operation_queue.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

struct Gate {
  std::mutex mutex;
  std::condition_variable event;
  bool open = false;
  void release() { std::lock_guard<std::mutex> lock(mutex); open = true; event.notify_all(); }
  void wait() { std::unique_lock<std::mutex> lock(mutex); event.wait(lock, [&] { return open; }); }
};

int main() {
  const auto unexpected = [](OperationFailure, const std::string&) { assert(false); };
  const auto idle = [](OperationQueue& queue) {
    const auto deadline = OperationQueue::Clock::now() + 2s;
    while (queue.stats().running || queue.stats().queued) {
      assert(OperationQueue::Clock::now() < deadline);
      std::this_thread::sleep_for(1ms);
    }
  };
  {
    OperationQueue queue({2, 16, 64u * 1024u * 1024u});
    Gate active;
    std::promise<void> started, independent, finished;
    std::vector<int> order;
    assert(queue.submit({"canonical", 4, OperationQueue::Clock::now() + 3s,
        [&] { started.set_value(); active.wait(); order.push_back(0); }, unexpected}));
    assert(started.get_future().wait_for(1s) == std::future_status::ready);
    for (int i = 1; i <= 16; ++i)
      assert(queue.submit({"canonical", 4, OperationQueue::Clock::now() + 3s,
          [&, i] { order.push_back(i); if (i == 16) finished.set_value(); }, unexpected}));
    bool overloaded = false;
    assert(!queue.submit({"canonical", 4, OperationQueue::Clock::now() + 3s, [] { assert(false); },
        [&](OperationFailure why, const auto&) { overloaded = why == OperationFailure::Overload; }}));
    assert(overloaded);
    assert(queue.submit({"independent", 4, OperationQueue::Clock::now() + 3s,
        [&] { independent.set_value(); }, unexpected}));
    assert(independent.get_future().wait_for(1s) == std::future_status::ready);
    active.release();
    assert(finished.get_future().wait_for(1s) == std::future_status::ready);
    idle(queue);
    queue.shutdown();
    assert(order.size() == 17);
    for (int i = 0; i <= 16; ++i) assert(order[i] == i);
    assert(queue.stats().overloaded == 1 && queue.stats().residentBytes == 0);
  }
  {
    OperationQueue queue({2, 16, 1024});
    Gate active;
    std::promise<void> started, expired, next;
    std::atomic<unsigned> expirations{0};
    assert(queue.submit({"canonical", 768, OperationQueue::Clock::now() + 100ms,
        [&] { started.set_value(); active.wait(); },
        [&](OperationFailure why, const auto&) {
          assert(why == OperationFailure::Deadline);
          assert(expirations.fetch_add(1) == 0);
          expired.set_value();
        }}));
    assert(started.get_future().wait_for(1s) == std::future_status::ready);
    bool overloaded = false;
    assert(!queue.submit({"other", 512, OperationQueue::Clock::now() + 3s, [] { assert(false); },
        [&](OperationFailure why, const auto&) { overloaded = why == OperationFailure::Overload; }}));
    assert(overloaded);
    assert(queue.submit({"canonical", 0, OperationQueue::Clock::now() + 3s,
        [&] { next.set_value(); }, unexpected}));
    assert(expired.get_future().wait_for(1s) == std::future_status::ready);
    auto successor = next.get_future();
    // Deadline response must not release the lane while a write is still running.
    assert(successor.wait_for(50ms) == std::future_status::timeout);
    assert(queue.stats().residentBytes == 1024);
    active.release();
    assert(successor.wait_for(1s) == std::future_status::ready);
    idle(queue);
    queue.shutdown();
    assert(queue.stats().expired == 1 && queue.stats().residentBytes == 0);
  }
  {
    OperationQueue queue({1, 16, 4096});
    Gate active;
    std::promise<void> started, expired;
    assert(queue.submit({"one", 1, OperationQueue::Clock::now() + 3s,
        [&] { started.set_value(); active.wait(); },
        [&](OperationFailure why, const auto&) { assert(why == OperationFailure::Stopped); active.release(); }}));
    assert(started.get_future().wait_for(1s) == std::future_status::ready);
    assert(queue.submit({"two", 1, OperationQueue::Clock::now() + 50ms, [] { assert(false); },
        [&](OperationFailure why, const auto&) { assert(why == OperationFailure::Deadline); expired.set_value(); }}));
    std::atomic<unsigned> cancellations{0};
    const auto cancelled = queue.submit({"one", 1, OperationQueue::Clock::now() + 3s, [] { assert(false); },
        [&](OperationFailure why, const auto&) { assert(why == OperationFailure::Cancelled); ++cancellations; }});
    queue.cancel(cancelled);
    queue.cancel(cancelled);
    assert(cancellations == 1);
    assert(expired.get_future().wait_for(1s) == std::future_status::ready);
    queue.shutdown();
    queue.shutdown();
    assert(queue.stats().residentBytes == 0 && queue.stats().queued == 0);
  }
}
