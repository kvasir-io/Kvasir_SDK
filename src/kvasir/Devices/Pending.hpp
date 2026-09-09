#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Kvasir::Device {

/// One request in flight on a queued bus, and how its result reaches the loop: the
/// bus's callback runs in the interrupt and only stamps the time and stores the result;
/// the driver's next turn takes it. The handshake every I2CDeviceBase driver used to
/// write for itself (with one of the copies counting errors twice).
///
///     if(submitSend(bytes, pending_.callback())) { state_ = waiting; }
///     ...
///     case waiting:
///         switch(pending_.take()) {
///         case Outcome::running: break;
///         case Outcome::ok:      ...; break;
///         case Outcome::failed:  ...; break;
///         }
template<typename I2c, typename Clock>
struct Pending {
    enum class Outcome : std::uint8_t { running, ok, failed };

    /// The callback to hand the bus: interrupt context, stamps and stores.
    auto callback() {
        return [this](typename I2c::Result r) { complete(r); };
    }

    void complete(typename I2c::Result r) {
        stampUs_
          = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
              .count();
        ok_.store(r == I2c::Result::succeeded, std::memory_order_relaxed);
        done_.store(true, std::memory_order_release);
    }

    /// Loop context: the outcome once, `running` until the callback has run.
    Outcome take() {
        if(!done_.load(std::memory_order_acquire)) { return Outcome::running; }
        done_.store(false, std::memory_order_relaxed);
        return ok_.load(std::memory_order_relaxed) ? Outcome::ok : Outcome::failed;
    }

    /// Forget a request that is being abandoned (a reset while one is in flight).
    void clear() { done_.store(false, std::memory_order_relaxed); }

    /// Microseconds on the clock's epoch at the last completion; written before the
    /// release-store, read after the acquire-load.
    [[nodiscard]] std::int64_t stampUs() const { return stampUs_; }

private:
    std::atomic<bool> done_{false};
    std::atomic<bool> ok_{false};
    std::int64_t      stampUs_{};
};

}   // namespace Kvasir::Device
