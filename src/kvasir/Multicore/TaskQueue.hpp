#pragma once

#include "kvasir/Atomic/Queue.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#ifndef __arm__
    #include <thread>
#endif

namespace Kvasir::Multicore {

namespace detail {
    // The wake-up pair. On the target the event register makes a sev() that lands between
    // "check" and "wfe" return immediately, so there is no lost wake-up. On the host (unit
    // tests) a yield is the closest thing.
    //
    // The checks in front of the wfe() loops below (Future::wait, run) are plain loads on
    // purpose: on the RP2350 a successful exclusive store sets this core's own event, so a
    // check that did an RMW or took a lock would make the wfe() return at once and the
    // loop spin (doc/multicore.md, "An exclusive store sets this core's own event").
#ifdef __arm__
    inline void sev() { asm volatile("sev" ::: "memory"); }

    inline void wfe() { asm volatile("wfe" ::: "memory"); }
#else
    inline void sev() {}

    inline void wfe() { std::this_thread::yield(); }
#endif
}   // namespace detail

// Post a callable on one core, run it on the other, get the result back through a future.
//
//   struct Core1TasksConfig {
//       static constexpr std::size_t slots       = 8;    // tasks in flight
//       static constexpr std::size_t captureSize = 32;   // bytes a task's captures may occupy
//       static constexpr std::size_t resultSize  = 16;   // bytes a result may occupy
//       // optional: using OverflowPolicy = Kvasir::Atomic::OverFlowPolicyIgnore;  (default: assert)
//   };
//   using Core1Tasks = Kvasir::Multicore::TaskQueue<Core1TasksConfig>;
//
//   // producer core
//   auto f = Core1Tasks::post([a, b] { return crc(a, b); });   // Future<std::uint32_t>
//   if(f.ready()) { ... }
//   auto v = f.get();                                          // sleeps in wfe() until done
//   f.waitUntil(Clock::now() + 5ms);                           // bounded, any Kvasir clock
//   Core1Tasks::post([] { blink(); });                         // dropping a future is fine
//
//   // consumer core
//   [[noreturn]] void core1Main() { Core1Tasks::run(); }       // a dedicated worker
//   while(true) { ...; Core1Tasks::poll(); ... }               // or one task per loop turn
//
//   using Core1 = SecondaryCore<&core1Main, Core1Tasks, ...>;  // reset on every (re)launch
//
// No heap, no RTTI, no exceptions: a fixed pool of slots, each with 8-byte-aligned storage
// for the captures and for the result. Captures must be trivially copyable and trivially
// destructible (indices and pointers to static objects, never owning handles); results
// too. Mutable lambdas are fine.
//
// One producer core and one consumer core per instance; the other direction is a second
// instance. Posting from an ISR, or from both thread and ISR on the producer core, needs a
// CriticalSection around post(): the free-slot scan and the ring push are single-producer.
//
// A dropped future does not cancel anything: the task still runs, and the worker frees
// the slot afterwards. That is the fire-and-forget pattern, not a leak.
template<typename Config>
struct TaskQueue {
    static constexpr std::size_t Slots       = Config::slots;
    static constexpr std::size_t CaptureSize = Config::captureSize;
    static constexpr std::size_t ResultSize  = Config::resultSize;

    static_assert(Slots > 0 && Slots < 255,
                  "1..254 slots; the index is a byte and 255 is 'none'");

    // What post() does when every slot is taken, before returning an invalid future.
    struct DefaultOverflow {
        using OverflowPolicy = Kvasir::Atomic::OverFlowPolicyAssert;
    };

    static constexpr bool HasOverflowPolicy = requires { typename Config::OverflowPolicy; };
    using OverflowPolicy =
      typename std::conditional_t<HasOverflowPolicy, Config, DefaultOverflow>::OverflowPolicy;

    static constexpr std::uint8_t Invalid = 0xFF;

    // Capture and result storage alignment. Not std::max_align_t: on the ARM target that
    // is 4 while a double or an int64 wants 8.
    static constexpr std::size_t SlotAlignment = 8;

    enum State : std::uint8_t { free, posted, running, done, abandoned };

    template<typename R>
    class Future {
    public:
        Future() = default;

        explicit Future(std::uint8_t index) : index_{index} {}

        Future(Future const&)            = delete;
        Future& operator=(Future const&) = delete;

        Future(Future&& other) noexcept
          : index_{std::exchange(other.index_,
                                 Invalid)} {}

        Future& operator=(Future&& other) noexcept {
            if(this != &other) {
                release();
                index_ = std::exchange(other.index_, Invalid);
            }
            return *this;
        }

        ~Future() { release(); }

        // False for the future post() returns when no slot was free.
        [[nodiscard]] bool valid() const { return index_ != Invalid; }

        // The task has run and its result is waiting.
        [[nodiscard]] bool ready() const {
            return valid() && slots[index_].state.load(std::memory_order_acquire) == done;
        }

        // Sleep until ready(). Every completion is followed by a sev().
        void wait() const {
            assert(valid());
            while(!ready()) { detail::wfe(); }
        }

        // Bounded wait on any clock with a static now(): true when ready before the deadline.
        template<typename TimePoint>
        [[nodiscard]] bool waitUntil(TimePoint deadline) const {
            assert(valid());
            while(!ready()) {
                if(TimePoint::clock::now() >= deadline) { return false; }
            }
            return true;
        }

        // Wait, take the result, free the slot.
        R get() {
            wait();
            return take();
        }

        // Take the result if it is ready; false (and `out` untouched) otherwise.
        template<typename U = R>
            requires(!std::is_void_v<U>)
        [[nodiscard]] bool tryGet(U& out) {
            if(!ready()) { return false; }
            out = take();
            return true;
        }

    private:
        std::uint8_t index_{Invalid};

        R take() {
            Slot& s = slots[index_];
            if constexpr(std::is_void_v<R>) {
                s.state.store(free, std::memory_order_release);
                index_ = Invalid;
            } else {
                R value;
                std::memcpy(&value, s.result.data(), sizeof(R));
                s.state.store(free, std::memory_order_release);
                index_ = Invalid;
                return value;
            }
        }

        // Give the slot up without collecting. Done: free it now. Posted or running: mark
        // it abandoned, the worker frees it after the task has run. The loop is for the
        // worker advancing the state between our load and our CAS.
        void release() {
            if(!valid()) { return; }
            Slot& s  = slots[index_];
            auto  st = s.state.load(std::memory_order_acquire);
            while(true) {
                if(st == done) {
                    s.state.store(free, std::memory_order_release);
                    break;
                }
                if(st == posted || st == running) {
                    if(s.state.compare_exchange_weak(st,
                                                     abandoned,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire))
                    {
                        break;
                    }
                    continue;   // st was reloaded by the failed CAS
                }
                break;   // free or abandoned: nothing of ours left in it
            }
            index_ = Invalid;
        }
    };

    // Producer core. Returns an invalid future if every slot is taken (after running the
    // overflow policy, which asserts by default).
    template<typename F>
    static auto post(F&& f) -> Future<std::invoke_result_t<std::remove_cvref_t<F>&>> {
        using Fn = std::remove_cvref_t<F>;
        using R  = std::invoke_result_t<Fn&>;

        static_assert(std::is_trivially_copyable_v<Fn> && std::is_trivially_destructible_v<Fn>,
                      "task captures must be trivially copyable and trivially destructible: pass "
                      "indices or pointers to static objects, not owning handles");
        static_assert(sizeof(Fn) <= CaptureSize, "task captures do not fit captureSize");
        static_assert(alignof(Fn) <= SlotAlignment, "task captures are overaligned");
        if constexpr(!std::is_void_v<R>) {
            static_assert(std::is_trivially_copyable_v<R>,
                          "task results must be trivially copyable");
            static_assert(sizeof(R) <= ResultSize, "task result does not fit resultSize");
            static_assert(alignof(R) <= SlotAlignment, "task result is overaligned");
        }

        auto const index = acquireSlot();
        if(index == Invalid) {
            OverflowPolicy{}();
            return Future<R>{};
        }

        Slot& s = slots[index];
        new(s.captures.data()) Fn{std::forward<F>(f)};
        s.invoke = [](std::byte* captures, std::byte* result) {
            auto& fn = *std::launder(reinterpret_cast<Fn*>(captures));
            if constexpr(std::is_void_v<R>) {
                static_cast<void>(result);
                fn();
            } else {
                new(result) R{fn()};
            }
        };
        pending.push(index);   // release: everything written above is visible to the worker
        detail::sev();
        return Future<R>{index};
    }

    // Consumer core: run at most one task. True if one ran.
    static bool poll() {
        std::uint8_t index{};
        if(!pending.pop_into(index)) { return false; }

        Slot& s = slots[index];

        // A dropped future may have moved the slot to `abandoned` already: the task still
        // runs (fire and forget), only the result goes nowhere.
        std::uint8_t expected     = posted;
        bool const   wasAbandoned = !s.state.compare_exchange_strong(expected,
                                                                     running,
                                                                     std::memory_order_acq_rel,
                                                                     std::memory_order_acquire);
        assert(!wasAbandoned || expected == abandoned);

        s.invoke(s.captures.data(), s.result.data());

        if(wasAbandoned) {
            s.state.store(free, std::memory_order_release);
        } else {
            expected = running;
            if(!s.state.compare_exchange_strong(expected,
                                                done,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire))
            {
                // Abandoned while running: nobody will collect, free it ourselves.
                assert(expected == abandoned);
                s.state.store(free, std::memory_order_release);
            }
        }
        detail::sev();
        return true;
    }

    // Consumer core: a dedicated worker.
    [[noreturn]] static void run() {
        while(true) {
            if(!poll()) { detail::wfe(); }
        }
    }

    // Every slot free and the ring empty. For a consumer core that is provably stopped:
    // a core reset mid-task leaves its slot `running` forever, a future waiting on it
    // spins forever, and freeSlots() shrinks by one per relaunch. Listing the TaskQueue in
    // the SecondaryCore's list runs this as primaryPrepare() before every launch; it has
    // no init steps of its own, so that costs nothing else. Futures still held by the
    // producer for tasks that were in flight are stale after this: they read `free` and
    // stay not-ready, so drop them.
    static void reset() {
        pending.clear();
        for(auto& s : slots) { s.state.store(free, std::memory_order_release); }
    }

    static void primaryPrepare() { reset(); }

    // Diagnostics.
    [[nodiscard]] static std::size_t pendingCount() { return pending.size(); }

    [[nodiscard]] static std::size_t freeSlots() {
        std::size_t n = 0;
        for(auto const& s : slots) {
            if(s.state.load(std::memory_order_relaxed) == free) { ++n; }
        }
        return n;
    }

private:
    struct Slot {
        alignas(SlotAlignment) std::array<std::byte,
                                          CaptureSize> captures{};
        alignas(SlotAlignment) std::array<std::byte,
                                          ResultSize> result{};
        void (*invoke)(std::byte*,
                       std::byte*){nullptr};
        std::atomic<std::uint8_t> state{free};
    };

    static inline std::array<Slot, Slots> slots{};

    // Capacity of a Queue<T, N> is N - 1.
    static inline Kvasir::Atomic::Queue<std::uint8_t,
                                        Slots + 1,
                                        Kvasir::Atomic::OverFlowPolicyIgnore,
                                        Kvasir::Atomic::SyncThread>
      pending{};

    static std::uint8_t acquireSlot() {
        for(std::size_t i = 0; i < Slots; ++i) {
            std::uint8_t expected = free;
            if(slots[i].state.compare_exchange_strong(expected,
                                                      posted,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire))
            {
                return static_cast<std::uint8_t>(i);
            }
        }
        return Invalid;
    }
};

}   // namespace Kvasir::Multicore
