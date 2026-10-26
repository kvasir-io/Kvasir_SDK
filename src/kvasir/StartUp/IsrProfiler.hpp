#pragma once

#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Mpl/List.hpp"
#include "kvasir/Port/Capabilities.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#if KVASIR_CORE_HAS_CYCLE_COUNTER
    #include "cortex_common/Debug.hpp"   // the DWT cycle counter
#endif

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

    // Per-ISR statistics storage, unique per <IsrIndex, TimeSource, Core>: a per-core vector (SysTick, the SIO FIFO)
    // profiled on both cores has one record per core.
    template<int IsrIndex, typename TimeSource, unsigned Core>
    struct IsrProfileStats {
        // --- interval tracking (time between consecutive ISR entries) ---
        static inline std::atomic<bool>          hasFirst{false};
        static inline std::atomic<std::uint32_t> callCount{0};
        static inline std::atomic<std::uint32_t> lastCallTime{0};
        static inline std::atomic<std::uint32_t> minInterval{
          std::numeric_limits<std::uint32_t>::max()};
        static inline std::atomic<std::uint32_t> maxInterval{0};
        static inline std::atomic<std::uint64_t> totalInterval{0};
        static inline std::atomic<std::uint32_t> intervals{0};   // the intervals in totalInterval

        // --- duration tracking (time spent inside the ISR) ---
        static inline std::atomic<std::uint32_t> minDuration{
          std::numeric_limits<std::uint32_t>::max()};
        static inline std::atomic<std::uint32_t> maxDuration{0};
        static inline std::atomic<std::uint64_t> totalDuration{0};
        static inline std::atomic<std::uint32_t> durations{0};   // the durations in totalDuration

        // Called from IsrProfileWrapper: enter = the time source before Original(), exit = after it. counted: the
        // counter ran up to enter (intervalCounted) and on to exit (durationCounted) - TimeSource::keepRunning()
        // before and after. A sample across a stretch where the counter stood still (a debugger cleared DEMCR.TRCENA)
        // is not taken: the call is counted, its interval or duration is not.
        static void record(std::uint32_t enter,
                           std::uint32_t exit,
                           bool          intervalCounted,
                           bool          durationCounted) noexcept {
            // Interval between consecutive ISR entries: needs a previous entry (hasFirst) the counter ran on from.
            std::uint32_t const last = lastCallTime.exchange(enter, std::memory_order_relaxed);
            callCount.fetch_add(1, std::memory_order_relaxed);
            if(intervalCounted && hasFirst.load(std::memory_order_relaxed)) {
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
                intervals.fetch_add(1, std::memory_order_relaxed);
            }
            // a counter that stood still inside this call leaves no entry to measure the next interval from
            hasFirst.store(durationCounted, std::memory_order_relaxed);
            if(!durationCounted) { return; }

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
            durations.fetch_add(1, std::memory_order_relaxed);
        }

        static IsrProfileSnapshot snapshot() noexcept {
            auto const count     = callCount.load(std::memory_order_relaxed);
            auto const intvTotal = totalInterval.load(std::memory_order_relaxed);
            auto const intvCount = intervals.load(std::memory_order_relaxed);
            auto const durTotal  = totalDuration.load(std::memory_order_relaxed);
            auto const durCount  = durations.load(std::memory_order_relaxed);
            return {IsrIndex,
                    count,
                    minInterval.load(std::memory_order_relaxed),
                    maxInterval.load(std::memory_order_relaxed),
                    intvCount > 0 ? static_cast<std::uint32_t>(intvTotal / intvCount) : 0,
                    lastCallTime.load(std::memory_order_relaxed),
                    minDuration.load(std::memory_order_relaxed),
                    maxDuration.load(std::memory_order_relaxed),
                    durCount > 0 ? static_cast<std::uint32_t>(durTotal / durCount) : 0};
        }
    };

    // Default time source: the DWT cycle counter (Cortex-M4/M33: the core package's cortex_common/Debug.hpp). The DWT
    // is in the core's private peripheral bus, so each core of a dual-core part has its own counter and starts it
    // itself (each core's IsrProfiling entry does). A core without one (Cortex-M0+: its DWT has watchpoints and PC
    // sampling only, no cycle counter) has the name only - a profiler there names its own time source.
#if KVASIR_CORE_HAS_CYCLE_COUNTER
    struct DwtTimeSource {
        static std::uint32_t now() noexcept { return Kvasir::Core::Debug::cycleCount(); }

        // Before any profiled ISR fires (IsrProfiling's preEnableRuntimeInit): TRCENA, CYCCNTENA; CYCCNT is never
        // written (its start value is unknown, every interval and duration is a difference of two reads)
        static void enable() noexcept { Kvasir::Core::Debug::enableCycleCounter(); }

        // Whether the counter ran since the last call, started again if not: a debugger clears DEMCR.TRCENA under a
        // running firmware (a J-Link session ending after a reset; cortex_common/Debug.hpp keepCycleCounterRunning),
        // and the samples across that stretch are dropped instead of read as 0 cycles. One load per call.
        [[gnu::always_inline]] static bool keepRunning() noexcept {
            return Kvasir::Core::Debug::keepCycleCounterRunning();
        }
    };
#else
    struct DwtTimeSource;
#endif

    // Unique wrapper type per (OriginalFn, InterruptIndex, TimeSource, Core).
    // Exposes `value` and `IType` identical to Nvic::Isr<F, Index<I>>,
    // so the vector table (VectorTable.hpp) and the list rules take it as one.
    template<Nvic::IsrFunctionPointer Original,
             typename IndexType,
             typename TimeSource,
             unsigned Core>
    struct IsrProfileWrapper {
        static void onIsr() noexcept {
            bool const          ranBefore = TimeSource::keepRunning();
            std::uint32_t const enter     = TimeSource::now();
            Original();
            std::uint32_t const exit = TimeSource::now();
            IsrProfileStats<IndexType::value, TimeSource, Core>::record(enter,
                                                                        exit,
                                                                        ranBefore,
                                                                        TimeSource::keepRunning());
        }

        static constexpr Nvic::IsrFunctionPointer value = &onIsr;
        using IType                                     = IndexType;
        using type                                      = IsrProfileWrapper;
        using Stats = IsrProfileStats<IndexType::value, TimeSource, Core>;
    };

    // Predicate: is T an IsrProfileWrapper?
    template<typename T>
    struct IsProfileWrapper : std::false_type {};

    template<Nvic::IsrFunctionPointer F, typename I, typename TS, unsigned Core>
    struct IsProfileWrapper<IsrProfileWrapper<F, I, TS, Core>> : std::true_type {};

    // -------------------------------------------------------------------
    // Policy types — control which ISR indices get wrapped
    // -------------------------------------------------------------------

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
    template<typename Policy, typename TimeSource, unsigned Core, typename IsrT>
    struct ApplyProfilingToIsr {
        using type = IsrT;
    };

    // Specialisation for Nvic::Isr<F, Index<I>>: wrap or pass through
    template<typename Policy, typename TimeSource, unsigned Core, Nvic::IsrFunctionPointer F, int I>
    struct ApplyProfilingToIsr<Policy, TimeSource, Core, Nvic::Isr<F, Nvic::Index<I>>> {
        using type = std::conditional_t<Policy::template ShouldProfile<I>::value,
                                        IsrProfileWrapper<F, Nvic::Index<I>, TimeSource, Core>,
                                        Nvic::Isr<F, Nvic::Index<I>>>;
    };

    template<typename Policy, typename TimeSource, unsigned Core, typename List>
    struct TransformIsrList;

    template<typename Policy, typename TimeSource, unsigned Core, typename... Ts>
    struct TransformIsrList<Policy, TimeSource, Core, brigand::list<Ts...>> {
        using type
          = brigand::list<typename ApplyProfilingToIsr<Policy, TimeSource, Core, Ts>::type...>;
    };

    // -------------------------------------------------------------------
    // Filter a brigand::list to only IsrProfileWrapper entries
    // -------------------------------------------------------------------

    template<typename List>
    using FilterWrappersT = MPL::Filter<MPL::Holds<IsProfileWrapper>, List>;

    // -------------------------------------------------------------------
    // The Startup list entry
    // -------------------------------------------------------------------

    namespace Detail {
        template<typename T>
        inline constexpr bool isCompleteTimeSource = requires { sizeof(T); };
    }   // namespace Detail

    // ISR profiling, listed in a Startup (or SecondaryCore) list like any other entry:
    //
    //   using Policy  = Kvasir::Startup::ProfileIndicesPolicy<Kvasir::Interrupt::i2c0>;
    //   using Startup = Kvasir::Startup::Startup<ClockSettings, ..., Kvasir::Startup::IsrProfiling<Policy>>;
    //   for(auto const& p : Kvasir::Startup::isrProfiles<Startup>()) { ... }
    //
    // Every vector the policy selects goes into the table as an IsrProfileWrapper (IsrTransform, Entry.hpp: the
    // checks see the list as written, only the table and the list's IsrList the wrappers). The time source starts in
    // preEnableRuntimeInit, on the core whose list it is, before that core's Core::unmaskIrqs(): interrupts are
    // masked until then, so no profiled ISR runs earlier. One per list (Checks.hpp). In a SecondaryCore's list it
    // profiles core 1's table: the stats are keyed by core too, isrProfiles<Core1>() reads them (from either core).
    template<typename Policy, typename TimeSource = DwtTimeSource>
    struct IsrProfiling {
        static_assert(Detail::isCompleteTimeSource<TimeSource>,
                      "IsrProfiling: this core has no DWT cycle counter "
                      "(KVASIR_CORE_HAS_CYCLE_COUNTER is 0): pass "
                      "a TimeSource with now(), enable() and keepRunning()");

        // The ISR list of core Core's table in, the list with the policy's vectors wrapped out.
        template<typename Isrs, unsigned Core>
        using IsrTransform = typename TransformIsrList<Policy, TimeSource, Core, Isrs>::type;

        static void preEnableRuntimeInit() { TimeSource::enable(); }

        // the same entry may be listed for both cores: each core wraps its own table and starts its own counter
        static constexpr bool perCoreEntry = true;
    };

    namespace Detail {
        struct ProfiledVector {
            int isrIndex;
            IsrProfileSnapshot (*snapshot)() noexcept;
        };

        // The wrappers' snapshot functions in vector-index order (= table order; the list is in list order).
        template<typename... Wrappers>
        consteval std::array<ProfiledVector,
                             sizeof...(Wrappers)>
        profiledVectorsOf(brigand::list<Wrappers...>) {
            std::array<ProfiledVector, sizeof...(Wrappers)> v{
              ProfiledVector{Wrappers::IType::value, &Wrappers::Stats::snapshot}
              ...
            };
            std::sort(v.begin(), v.end(), [](ProfiledVector const& a, ProfiledVector const& b) {
                return a.isrIndex < b.isrIndex;
            });
            return v;
        }
    }   // namespace Detail

    // How many vectors of S's table are profiled (the size of isrProfiles<S>()).
    template<typename S>
    inline constexpr std::size_t isrProfiledCount
      = brigand::size<FilterWrappersT<typename S::IsrList>>::value;

    // One snapshot per profiled vector of S's table (S::IsrList, an IsrProfiling entry's wrappers), in table order.
    // S is a Startup or a SecondaryCore.
    template<typename S>
    std::array<IsrProfileSnapshot,
               isrProfiledCount<S>>
    isrProfiles() noexcept {
        static constexpr auto vectors
          = Detail::profiledVectorsOf(FilterWrappersT<typename S::IsrList>{});
        std::array<IsrProfileSnapshot, vectors.size()> out{};
        for(std::size_t i = 0; i < vectors.size(); ++i) { out[i] = vectors[i].snapshot(); }
        return out;
    }

}}   // namespace Kvasir::Startup
