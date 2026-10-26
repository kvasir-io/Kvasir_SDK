#pragma once

#include "kvasir/Atomic/Atomic.hpp"

#include <atomic>

namespace Kvasir::Atomic {

// A software spinlock on the architecture's exclusive accesses (ldaexb/strexb/stlb on
// Armv8-M mainline). Not the SIO hardware spinlocks: erratum RP2350-E2 makes writes to the
// doorbell and MTIME registers release those spuriously, so pico-sdk and this SDK both
// stay away from them (RP2350 datasheet 3.1.4 / RP2350-E2, md l.1903, l.66724).
//
// Exclusives only arbitrate between cores through the bus fabric's global monitor, and a
// Cortex-M33 only sends them there for shareable memory - which, by default, nothing is.
// The chip's startup sets ACTLR.EXTEXCLALL on every core in a multicore build so that all
// exclusives go global: bit 29, "0 = ... only assert HEXCL and respond to HEXOKAY if the
// address is shareable", 1 = all of them (Cortex-M33 TRM 100230 B2.2, md l.1788; RP2350
// datasheet Table 188 ACTLR, md l.8655). Measured without it: two cores doing fetch_add on one word keep
// half the increments, and this lock holds or fails depending on which core is a cycle
// ahead. With it: exact counts, every time.
//
// On the RP2040, which has no exclusives, the flag's test_and_set goes through the atomic
// shim, whose cross-core lock is an SIO spinlock (the chip's Port::CrossCoreLock), while
// unlock()'s clear() is a plain store outside that lock. Correct because the shim's exchange
// writes only when the value changes: a waiter never writes 1 over a flag its owner has just
// cleared (StartUp/Runtime/LibAtomicBlocks.hpp). A few times slower than the inline ldaexb/strexb.
//
// Satisfies Lockable: use it with std::lock_guard or std::scoped_lock.
//
// Rules, because a spinlock that is not held briefly is a deadlock waiting for its moment:
//   - hold it for microseconds and never call out from under it;
//   - never take two spinlocks on one core, in any order;
//   - it does not mask interrupts. If an ISR on *either* core touches the same data, use
//     IrqSpinlock below instead, otherwise the ISR spins on the lock its own thread
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
// masked before the lock is taken and PRIMASK is back as found after it is released, so the lock is
// never held by something an interrupt could pre-empt. This is the lock for data shared
// with an ISR, on the same core or the other one. Lockable, like Spinlock. (The critical-section API is
// criticalSection<Tag>() in CriticalSection.hpp; its two-core policy holds one of these per tag.)
struct IrqSpinlock {
    Spinlock       lock_{};
    Core::IrqState found_{};   // PRIMASK as lock() found it; unlock() writes it back (msr + isb)

    void lock() {
        auto const found = Core::IrqState::saveAndMask();
        lock_.lock();
        found_ = found;
    }

    [[nodiscard]] bool try_lock() {
        auto const found = Core::IrqState::saveAndMask();
        if(lock_.try_lock()) {
            found_ = found;
            return true;
        }
        found.restore();
        return false;
    }

    void unlock() {
        auto const found = found_;
        lock_.unlock();
        found.restore();
    }
};

}   // namespace Kvasir::Atomic
