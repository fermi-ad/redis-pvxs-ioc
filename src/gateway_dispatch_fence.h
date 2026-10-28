#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace redis_pvxs_ioc {

// Serializes changes of upstream connection generation with scheduling a PUT.
// The callback passed to dispatch() must not wait for PVXS or acquire the
// monitor mutex. Monitor callbacks acquire the monitor mutex before this fence;
// cancellation and PV close happen only after the fence has been released.
class GatewayDispatchFence {
public:
  std::optional<uint64_t> admit() const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!alive_ || !active_ || !connected_) return std::nullopt;
    return epoch_;
  }

  bool current(uint64_t epoch) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return alive_ && active_ && connected_ && epoch_ == epoch;
  }

  bool alive() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return alive_;
  }

  bool connected() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return alive_ && connected_;
  }

  void activate() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!alive_ || retired_) throw std::logic_error("cannot activate a retired gateway");
    active_ = true;
  }

  void retire() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    active_ = false;
    retired_ = true;
    ++epoch_;
  }

  void stop() noexcept {
    std::lock_guard<std::mutex> guard(mutex_);
    alive_ = false;
    connected_ = false;
  }

  void connect() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (alive_) connected_ = true;
  }

  void disconnect() {
    std::lock_guard<std::mutex> guard(mutex_);
    connected_ = false;
    ++epoch_;
  }

  template <typename F>
  bool dispatch(uint64_t epoch, F&& send) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!alive_ || !active_ || !connected_ || epoch_ != epoch) return false;
    std::forward<F>(send)();
    return true;
  }

private:
  mutable std::mutex mutex_;
  bool alive_ = true, active_ = false, retired_ = false, connected_ = false;
  uint64_t epoch_ = 0;
};

} // namespace redis_pvxs_ioc
