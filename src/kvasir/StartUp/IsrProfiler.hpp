#pragma once

#include "kvasir/Common/Interrupt.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace Kvasir { namespace Startup {

    // Snapshot of statistics for one ISR at a point in time
    struct IsrProfileSnapshot {
        int           isrIndex;
        std::uint32_t callCount;
        std::uint32_t minIntervalCycles;
        std::uint32_t maxIntervalCycles;
        std::uint32_t avgIntervalCycles;
        std::uint32_t lastCallTime;
        std::uint32_t minDurationCycles;
        std::uint32_t maxDurationCycles;
        std::uint32_t avgDurationCycles;
    };

    // Per-ISR statistics storage, unique per <IsrIndex, TimeSource>
    template<int IsrIndex, typename TimeSource>
    struct IsrProfileStats {
        // --- interval tracking (time between consecutive ISR entries) ---
        static inline std::atomic<bool>          hasFirst{false};
        static inline std::atomic<std::uint32_t> callCount{0};
        static inline std::atomic<std::uint32_t> lastCallTime{0};
        static inline std::atomic<std::uint32_t> minInterval{
          std::numeric_limits<std::uint32_t>::max()};
        static inline std::atomic<std::uint32_t> maxInterval{0};
        static inline std::atomic<std::uint64_t> totalInterval{0};

        // --- duration tracking (time spent inside the ISR) ---
        static inline std::atomic<std::uint32_t> minDuration{
          std::numeric_limits<std::uint32_t>::max()};
        static inline std::atomic<std::uint32_t> maxDuration{0};
        static inline std::atomic<std::uint64_t> totalDuration{0};

        // Called from IsrProfileWrapper: enter = DWT before Original(),
        //                                exit  = DWT after Original().
        static void record(std::uint32_t enter,
                           std::uint32_t exit) noexcept {
            // Interval between consecutive ISR entries.
            // hasFirst guards against the CYCCNT=0 wraparound false-positive
            // that the old `last != 0` check suffered from.
            std::uint32_t const last = lastCallTime.exchange(enter, std::memory_order_relaxed);
            callCount.fetch_add(1, std::memory_order_relaxed);
            if(hasFirst.load(std::memory_order_relaxed)) {
                std::uint32_t const interval = enter - last;
                std::uint32_t       old      = minInterval.load(std::memory_order_relaxed);
                while(
                  interval < old
                  && !minInterval.compare_exchange_weak(old, interval, std::memory_order_relaxed))
                {}
                old = maxInterval.load(std::memory_order_relaxed);
                while(
                  interval > old
                  && !maxInterval.compare_exchange_weak(old, interval, std::memory_order_relaxed))
                {}
                totalInterval.fetch_add(interval, std::memory_order_relaxed);
            } else {
                hasFirst.store(true, std::memory_order_relaxed);
            }

            // Duration of this ISR invocation.
            std::uint32_t const duration = exit - enter;
            std::uint32_t       old      = minDuration.load(std::memory_order_relaxed);
            while(duration < old
                  && !minDuration.compare_exchange_weak(old, duration, std::memory_order_relaxed))
            {}
            old = maxDuration.load(std::memory_order_relaxed);
            while(duration > old
                  && !maxDuration.compare_exchange_weak(old, duration, std::memory_order_relaxed))
            {}
            totalDuration.fetch_add(duration, std::memory_order_relaxed);
        }

        static IsrProfileSnapshot snapshot() noexcept {
            auto const count     = callCount.load(std::memory_order_relaxed);
            auto const intvTotal = totalInterval.load(std::memory_order_relaxed);
            auto const durTotal  = totalDuration.load(std::memory_order_relaxed);
            // Interval avg uses (count - 1) because N calls produce N-1 intervals
            auto const intvCount = count > 1 ? count - 1 : 0;
            return {IsrIndex,
                    count,
                    minInterval.load(std::memory_order_relaxed),
                    maxInterval.load(std::memory_order_relaxed),
                    intvCount > 0 ? static_cast<std::uint32_t>(intvTotal / intvCount) : 0,
                    lastCallTime.load(std::memory_order_relaxed),
                    minDuration.load(std::memory_order_relaxed),
                    maxDuration.load(std::memory_order_relaxed),
                    count > 0 ? static_cast<std::uint32_t>(durTotal / count) : 0};
        }
    };

    // Default time source: ARM DWT cycle counter (Cortex-M3/M4/M7/M33)
    struct DwtTimeSource {
        static std::uint32_t now() noexcept {
            return *reinterpret_cast<std::uint32_t const volatile*>(0xE0001004U);   // DWT_CYCCNT
        }

        // Must be called once before any ISR fires (before Nvic::enable_all)
        static void enable() noexcept {
            // Enable trace subsystem: CoreDebug->DEMCR TRCENA bit
            *reinterpret_cast<std::uint32_t volatile*>(0xE000EDFC) |= (1U << 24);
            // Reset and enable cycle counter: DWT_CTRL CYCCNTENA bit
            *reinterpret_cast<std::uint32_t volatile*>(0xE0001004) = 0;   // reset CYCCNT
            *reinterpret_cast<std::uint32_t volatile*>(0xE0001000) |= 1U;
        }
    };

    // Unique wrapper type per (OriginalFn, InterruptIndex, TimeSource).
    // Exposes `value` and `IType` identical to Nvic::Isr<F, Index<I>>
    // so CompileIsrPointerList's lookup works unchanged.
    template<Nvic::IsrFunctionPointer Original,
             typename IndexType,
             typename TimeSource = DwtTimeSource>
    struct IsrProfileWrapper {
        static void onIsr() noexcept {
            std::uint32_t const enter = TimeSource::now();
            Original();
            IsrProfileStats<IndexType::value, TimeSource>::record(enter, TimeSource::now());
        }

        static constexpr Nvic::IsrFunctionPointer value = &onIsr;
        using IType                                     = IndexType;
        using type                                      = IsrProfileWrapper;
    };

    // Predicate: is T an IsrProfileWrapper?
    template<typename T>
    struct IsProfileWrapper : std::false_type {};

    template<Nvic::IsrFunctionPointer F, typename I, typename TS>
    struct IsProfileWrapper<IsrProfileWrapper<F, I, TS>> : std::true_type {};

    // -------------------------------------------------------------------
    // Policy types — control which ISR indices get wrapped
    // -------------------------------------------------------------------

    struct ProfileNonePolicy {
        template<int>
        struct ShouldProfile : std::false_type {};
    };

    struct ProfileAllPolicy {
        template<int>
        struct ShouldProfile : std::true_type {};
    };

    // Pass interrupt constants directly, e.g.:
    //   ProfileIndicesPolicy<Kvasir::Interrupt::i2c0, Kvasir::Interrupt::uart0>
    // std::remove_cv_t strips the 'const' that static constexpr members carry.
    template<auto... Interrupts>
    struct ProfileIndicesPolicy {
        template<int I>
        struct ShouldProfile
          : std::bool_constant<((std::remove_cv_t<decltype(Interrupts)>::value == I) || ...)> {};
    };

    template<auto... Interrupts>
    struct ProfileAllExceptPolicy {
        template<int I>
        struct ShouldProfile
          : std::bool_constant<((std::remove_cv_t<decltype(Interrupts)>::value != I) && ...)> {};
    };

    // -------------------------------------------------------------------
    // ISR list transformation
    // -------------------------------------------------------------------

    // Pass-through for anything that is not a plain Nvic::Isr<F, Index<I>>
    template<typename Policy, typename TimeSource, typename IsrT>
    struct ApplyProfilingToIsr {
        using type = IsrT;
    };

    // Specialisation for Nvic::Isr<F, Index<I>>: wrap or pass through
    template<typename Policy, typename TimeSource, Nvic::IsrFunctionPointer F, int I>
    struct ApplyProfilingToIsr<Policy, TimeSource, Nvic::Isr<F, Nvic::Index<I>>> {
        using type = std::conditional_t<Policy::template ShouldProfile<I>::value,
                                        IsrProfileWrapper<F, Nvic::Index<I>, TimeSource>,
                                        Nvic::Isr<F, Nvic::Index<I>>>;
    };

    template<typename Policy, typename TimeSource, typename List>
    struct TransformIsrList;

    template<typename Policy, typename TimeSource, typename... Ts>
    struct TransformIsrList<Policy, TimeSource, brigand::list<Ts...>> {
        using type = brigand::list<typename ApplyProfilingToIsr<Policy, TimeSource, Ts>::type...>;
    };

    // -------------------------------------------------------------------
    // Filter a brigand::list to only IsrProfileWrapper entries
    // -------------------------------------------------------------------

    namespace Detail {
        template<typename Out, typename List>
        struct FilterWrappersImpl;

        template<typename Out>
        struct FilterWrappersImpl<Out, brigand::list<>> {
            using type = Out;
        };

        template<typename... Os, typename T, typename... Ts>
        struct FilterWrappersImpl<brigand::list<Os...>, brigand::list<T, Ts...>>
          : FilterWrappersImpl<std::conditional_t<IsProfileWrapper<T>::value,
                                                  brigand::list<Os..., T>,
                                                  brigand::list<Os...>>,
                               brigand::list<Ts...>> {};
    }   // namespace Detail

    template<typename List>
    using FilterWrappersT = typename Detail::FilterWrappersImpl<brigand::list<>, List>::type;

}}   // namespace Kvasir::Startup
