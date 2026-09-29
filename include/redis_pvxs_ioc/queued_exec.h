#pragma once

#include "redis_pvxs_ioc/operation_queue.h"
#include <atomic>
#include <mutex>
#include <pvxs/source.h>

namespace redis_pvxs_ioc {

// Own the PVA handle through cancellation, queueing, timeout and completion.
// Network workers never hold this mutex while talking to a backend.
class QueuedExec : public std::enable_shared_from_this<QueuedExec> {
public:
  static std::shared_ptr<QueuedExec> create(std::unique_ptr<pvxs::server::ExecOp> op,
                                           const std::shared_ptr<OperationQueue>& queue) {
    auto result = std::shared_ptr<QueuedExec>(new QueuedExec(std::move(op), queue));
    std::weak_ptr<QueuedExec> weak = result;
    auto handle = result->op_;
    handle->onCancel([weak] {
      if (auto state = weak.lock()) {
        state->finish("", true);
        auto queue = state->queue_.lock();
        if (queue) queue->cancel(state->ticket_.load());
      }
    });
    return result;
  }

  bool stopped() const { return stopped_.load(); }
  void reply() { finish(""); }
  void error(const std::string& message) { finish(message); }
  void ticket(const std::shared_ptr<OperationQueue>& queue, uint64_t id) {
    // The ID may arrive after an immediate peer cancellation.
    ticket_ = id;
    if (aborted_) queue->cancel(id);
  }
  void wake(std::function<void()> fn) {
    bool notify;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      wake_ = fn;
      notify = stopped();
    }
    if (notify) fn();
  }
  std::shared_ptr<pvxs::server::ExecOp> operation() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return op_;
  }

private:
  QueuedExec(std::unique_ptr<pvxs::server::ExecOp> op, const std::shared_ptr<OperationQueue>& queue)
      : op_(std::move(op)), queue_(queue) {}
  void finish(const std::string& message, bool cancelled = false) {
    std::shared_ptr<pvxs::server::ExecOp> op;
    std::function<void()> notify;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (stopped_.exchange(true)) return;
      aborted_ = cancelled || !message.empty();
      op.swap(op_);
      notify = wake_;
    }
    if (notify) notify();
    if (op && !cancelled) {
      if (message.empty()) op->reply();
      else op->error(message);
    }
  }
  mutable std::mutex mutex_;
  std::shared_ptr<pvxs::server::ExecOp> op_;
  std::function<void()> wake_;
  std::atomic<bool> stopped_{false};
  std::atomic<bool> aborted_{false};
  std::weak_ptr<OperationQueue> queue_;
  std::atomic<uint64_t> ticket_{0};
};

} // namespace redis_pvxs_ioc
