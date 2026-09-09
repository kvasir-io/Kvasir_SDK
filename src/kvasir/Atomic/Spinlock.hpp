#pragma once

#include "kvasir/Atomic/Atomic.hpp"

#include <atomic>

namespace Kvasir::Atomic {

// A software spinlock on the architecture's exclusive accesses (ldaexb/strexb/stlb on
// Armv8-M mainline). Not the SIO hardware spinlocks: erratum RP2350-E2 makes writes to the
// doorbell and MTIME registers release those spuriously, so pico-sdk and this SDK both
// stay away from them.
//
// Exclusives only arbitrate between cores through the bus fabric's global monitor, and a
// Cortex-M33 only sends them there for shareable memory - which, by default, nothing is.
// The chip's startup sets ACTLR.EXTEXCLALL on every core in a multicore build so that all
// exclusives go global. Measured without it: two cores doing fetch_add on one word keep
// half the increments, and this lock holds or fails depending on which core is a cycle
// ahead. With it: exact counts, every time.
//
// On the RP2040, which has no exclusives, the flag's test_and_set goes through the atomic
// shim, whose cross-core lock is an SIO spinlock (chip/CrossCoreLock.hpp): correct, and a
// few times slower than the inline ldaexb/strexb.
//
// Satisfies Lockable: use it with std::lock_guard or std::scoped_lock.
//
// Rules, because a spinlock that is not held briefly is a deadlock waiting for its moment:
//   - hold it for microseconds and never call out from under it;
//   - never take two spinlocks on one core, in any order;
//   - it does not mask interrupts. If an ISR on *either* core touches the same data, use
//     CriticalSection below instead, otherwise the ISR spins on the lock its own thread
//     context holds and that core is gone.
struct Spinlock {
    std::atomic_flag flag{};

    void lock() {
        while(flag.test_and_set(std::memory_order_acquire)) {}
    }

    [[nodiscard]] bool try_lock() { return !flag.test_and_set(std::memory_order_acquire); }

    void unlock() { flag.clear(std::memory_order_release); }
};

// RAII for any of the locks here. std::lock_guard works too, but clang's thread-safety
// analysis annotates it and then wants the caller annotated as well; this one is quiet.
template<typename Lock>
struct LockGuard {
    Lock& lock;

    explicit LockGuard(Lock& l) : lock{l} { lock.lock(); }

    ~LockGuard() { lock.unlock(); }

    LockGuard(LockGuard const&)            = delete;
    LockGuard& operator=(LockGuard const&) = delete;
};

template<typename Lock>
LockGuard(Lock&) -> LockGuard<Lock>;

// Spinlock plus this core's interrupt mask, in that order of unwinding: interrupts are
// disabled before the lock is taken and re-enabled after it is released, so the lock is
// never held by something an interrupt could pre-empt. This is the lock for data shared
// with an ISR, on the same core or the other one. Lockable, like Spinlock.
struct CriticalSection {
    Spinlock lock_{};
    bool     wasEnabled_{false};

    void lock() {
        bool const enabled = Nvic::disable_all_and_get_old_state();
        lock_.lock();
        wasEnabled_ = enabled;
    }

    [[nodiscard]] bool try_lock() {
        bool const enabled = Nvic::disable_all_and_get_old_state();
        if(lock_.try_lock()) {
            wasEnabled_ = enabled;
            return true;
        }
        if(enabled) { Nvic::enable_all(); }
        return false;
    }

    void unlock() {
        bool const enabled = wasEnabled_;
        lock_.unlock();
        if(enabled) { Nvic::enable_all(); }
    }
};

}   // namespace Kvasir::Atomic
