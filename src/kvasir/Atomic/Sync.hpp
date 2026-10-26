#pragma once
// How an Atomic container orders its commit against the data it commits (Queue, Fifo, BipBuffer), the default a build
// gets from KVASIR_MULTICORE, and the index type of a ring.
//
// SyncSignal is the original: relaxed index accesses and a compiler-only fence. Exact for one core (the producer and
// consumer are the same core's thread and ISR, and a core never reorders its own memory accesses against an
// interrupt). Free.
//
// SyncThread makes the commit a release and the peek an acquire, one dmb each on Armv8-M, so a consumer on the other
// core sees the data the index promises.
//
// fence() goes between the data and the commit that publishes it (or frees it), acquire() between reading an index and
// the data it covers. The relaxed index loads order nothing by themselves: without acquire() the compiler may read a
// slot before the index that says it is filled (it may always load data_[head]; nothing makes that load wait for
// tail_).
//
// The ABI tag (KVASIR_ATOMIC_ABI): these policies, their default and the critical-section policies
// (CriticalSection.hpp) live in an inline namespace named after KVASIR_MULTICORE, `multicore` or `singlecore`. Every
// symbol whose mangled name carries one - each Queue, Fifo and BipBuffer instantiation and its out-of-line members, a
// function that takes or returns one, a template instantiated over one - is another symbol in a translation unit
// built with the other setting, so where two such units share it the link fails instead of a multicore image running
// a single-core queue. A mangled name cannot carry everything (as with libstdc++'s __cxx11 tag): an extern variable of
// a queue type, or an inline function whose own name hides the queue or section it uses, stays unchecked. Every
// translation unit of a Kvasir firmware gets the same setting (util.cmake defines it PUBLIC for CORE1_STACK_SIZE).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
    #define KVASIR_ATOMIC_ABI multicore
#else
    #define KVASIR_ATOMIC_ABI singlecore
#endif

namespace Kvasir::Atomic {
inline namespace KVASIR_ATOMIC_ABI {
    // the fences both policies use
    struct SyncFences {
        static void fence() { std::atomic_signal_fence(std::memory_order_release); }

        static void acquire() { std::atomic_signal_fence(std::memory_order_acquire); }
    };

    struct SyncSignal : SyncFences {
        static constexpr auto loadMemoryOrder{std::memory_order_relaxed};
        static constexpr auto storeMemoryOrder{std::memory_order_relaxed};
    };

    struct SyncThread : SyncFences {
        static constexpr auto loadMemoryOrder{std::memory_order_acquire};
        static constexpr auto storeMemoryOrder{std::memory_order_release};
    };

    // A multicore build (CORE1_STACK_SIZE set, KVASIR_MULTICORE defined) gets cross-core correct containers by
    // default; a single-core build keeps its exact old code.
#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
    using DefaultSync = SyncThread;
#else
    using DefaultSync = SyncSignal;
#endif
}   // namespace KVASIR_ATOMIC_ABI

namespace Detail {
    // the smallest unsigned type that holds 0 .. N (a Queue's indices run to Size - 1, BipBuffer's to Size)
    template<std::size_t N>
    using IndexFor
      = std::conditional_t<(N <= 0xFF),
                           std::uint8_t,
                           std::conditional_t<(N <= 0xFFFF), std::uint16_t, std::uint32_t>>;
}   // namespace Detail
}   // namespace Kvasir::Atomic
