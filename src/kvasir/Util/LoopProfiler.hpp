#pragma once
// What each main-loop entry costs: a profiler injected around every hook function call and every whole
// run<Hook>() / runTurn<Hook>() (StartUp/Hooks.hpp), with per-entry count / average / worst and the loop's period,
// slips and busy share, logged by LoopReport.
//
//     struct LoopProfile {
//         using TimeSource = Kvasir::Profile::DwtCyclesAt<HW::ClockSpeed>;   // M33/M4; SystickCycles<..> on M0+
//         static constexpr auto turnBudget = 1ms;                            // a turn longer than this is a slip
//     };
//     template<>
//     inline constexpr auto Kvasir::Startup::injectedHookProfiler<> = Kvasir::Profile::HookProfiler<LoopProfile>{};
//     using Startup = Kvasir::Startup::Startup<..., LoopProfile::TimeSource, Kvasir::Profile::LoopReport<LoopProfile,
//                                              ReportPeriod>, ...>;
//
// The specialisation must be the same in every translation unit (the header they all include), as for the
// critical-section policy. Off (not specialised) every call is the plain call it was.
//
// Time sources (a 32-bit free-running counter; deltas are wrap-safe while an interval stays under one wrap):
//   DwtCyclesAt<Hz>        DWT_CYCCNT, Armv7-M / Armv8-M Mainline; a Startup-list entry that switches it on
//   SystickCycles<Clock>   the low word of Kvasir's SysTick clock on the processor clock: core cycles, Armv6-M too
//   TimerMicros<Clock>     the RP TIMER's raw low word: 1 us, any core
//
// Config: TimeSource (required), hooks (a brigand::list, default Hook::MainLoop), turnBudget (a duration; zero:
// no slips).
#include "kvasir/StartUp/Hooks.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Profile {

template<typename T>
concept TimeSource = requires {
    { T::now() } -> std::same_as<std::uint32_t>;
    { T::ticksPerSecond } -> std::convertible_to<std::uint64_t>;
};

#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
/// DWT_CYCCNT. Its runtimeInit sets DEMCR.TRCENA (bit 24) and DWT_CTRL.CYCCNTENA (bit 0); Armv8-M ARM DDI0553B.y
/// D1.2.59 (DWT_CTRL) / D1.2.38 (DEMCR), Armv7-M ARM DDI0403E C1.8.7.
template<std::uint64_t CoreHz>
struct DwtCyclesAt {
    static constexpr std::uint64_t    ticksPerSecond = CoreHz;
    static constexpr std::string_view name           = "DWT_CYCCNT";

    [[gnu::always_inline]] static std::uint32_t now() {
        return *reinterpret_cast<std::uint32_t const volatile*>(0xE000'1004U);
    }

    static void runtimeInit() {
        *reinterpret_cast<std::uint32_t volatile*>(0xE000'EDFCU) |= 1U << 24U;   // DEMCR.TRCENA
        *reinterpret_cast<std::uint32_t volatile*>(0xE000'1004U) = 0;            // DWT_CYCCNT
        *reinterpret_cast<std::uint32_t volatile*>(0xE000'1000U) |= 1U;   // DWT_CTRL.CYCCNTENA
    }
};
#endif

/// The low word of a SysTick clock on the processor clock: core cycles (Kvasir's SysTick clock extends the 24-bit
/// counter by its overflow count, so deltas past one reload are right too). Read on the core that owns it.
template<typename SystickClock>
struct SystickCycles {
    static constexpr std::uint64_t ticksPerSecond
      = SystickClock::period::den / SystickClock::period::num;
    static constexpr std::string_view name = "SysTick";

    [[gnu::always_inline]] static std::uint32_t now() {
        return static_cast<std::uint32_t>(SystickClock::now().time_since_epoch().count());
    }
};

/// A clock's low word in its own ticks (the RP TIMER: microseconds, any core).
template<typename Clock>
struct TimerMicros {
    static constexpr std::uint64_t    ticksPerSecond = Clock::period::den / Clock::period::num;
    static constexpr std::string_view name           = "TIMER";

    [[gnu::always_inline]] static std::uint32_t now() {
        return static_cast<std::uint32_t>(Clock::now().time_since_epoch().count());
    }
};

struct EntryStats {
    std::uint32_t count{};
    std::uint32_t worstTicks{};
    std::uint32_t lastTicks{};
    std::uint32_t
      busyTurns{};   // runTurn: turns this entry said "busy" (it keeps a sleeping loop awake)
    std::uint64_t    totalTicks{};   // two words: a debugger read may tear it
    std::string_view name{};
    EntryStats*      next{};   // the report's walk: linked the first time the entry runs
    bool             linked{};
};

struct LoopStats {
    std::uint32_t turns{};
    std::uint32_t worstPeriodTicks{};   // run<Hook>() start to start
    std::uint32_t slips{};              // turns longer than turnBudget
    std::uint32_t lastStart{};
    std::uint64_t hookTicks{};     // inside run<Hook>()
    std::uint64_t periodTicks{};   // start to start, summed
    bool          started{};
};

namespace Detail {
    template<auto Fn>
    constexpr std::string_view rawName() {
        return __PRETTY_FUNCTION__;
    }

    // "Fn = &Ns::Class<Args>::fn" (clang) / "auto Fn = Ns::Class<Args>::fn" (gcc) -> "Class::fn": template arguments
    // dropped, the last two names kept
    template<auto Fn>
    constexpr std::string_view shortName() {
        constexpr std::string_view f = rawName<Fn>();
        constexpr auto             b = f.find("Fn = ");
        if constexpr(b == std::string_view::npos) {
            return f;
        } else {
            constexpr auto   e = f.find_first_of(";]", b + 5);
            std::string_view n = f.substr(b + 5, e - b - 5);
            if(!n.empty() && n.front() == '&') { n.remove_prefix(1); }
            return n;
        }
    }

    template<auto Fn>
    struct EntryName {
        static constexpr std::string_view full = shortName<Fn>();

        struct Buf {
            char        data[full.size() + 1]{};
            std::size_t size{};
        };

        // drop <...> (nested), then keep the last two '::' parts
        static constexpr Buf storage = [] {
            Buf flat{};
            int depth = 0;
            for(char const c : full) {
                if(c == '<') {
                    ++depth;
                } else if(c == '>') {
                    --depth;
                } else if(depth == 0) {
                    flat.data[flat.size++] = c;
                }
            }
            std::string_view s{flat.data, flat.size};
            auto const       last = s.rfind("::");
            if(last != std::string_view::npos && last > 0) {
                auto const prev = s.rfind("::", last - 1);
                if(prev != std::string_view::npos) { s.remove_prefix(prev + 2); }
            }
            Buf out{};
            for(char const c : s) { out.data[out.size++] = c; }
            return out;
        }();
        static constexpr std::string_view value{storage.data, storage.size};
    };
}   // namespace Detail

/// "Class::fn" for an entry's function; a firmware may specialise it.
template<auto Fn>
inline constexpr std::string_view entryName = Detail::EntryName<Fn>::value;

template<typename Config>
struct HookProfiler {
    using TS = typename Config::TimeSource;
    static_assert(TimeSource<TS>,
                  "LoopProfile::TimeSource: now() -> uint32, ticksPerSecond");

private:
    static constexpr auto hooksOf() {
        if constexpr(requires { typename Config::hooks; }) {
            return static_cast<typename Config::hooks*>(nullptr);
        } else {
            return static_cast<brigand::list<Kvasir::Hook::MainLoop>*>(nullptr);
        }
    }

    template<typename H,
             typename... Hs>
    static constexpr bool contains(brigand::list<Hs...>*) {
        return (std::is_same_v<H, Hs> || ...);
    }

    static constexpr std::uint64_t budgetTicks = [] {
        if constexpr(requires { Config::turnBudget; }) {
            return static_cast<std::uint64_t>(
              std::chrono::duration_cast<
                std::chrono::duration<std::uint64_t, std::ratio<1, 1'000'000'000>>>(
                Config::turnBudget)
                .count()
              * TS::ticksPerSecond / 1'000'000'000ULL);
        } else {
            return std::uint64_t{0};
        }
    }();

public:
    template<typename Hook>
    static constexpr bool profiles = contains<Hook>(hooksOf());

    template<typename Hook, typename E>
    static inline EntryStats stats{.name = entryName<E::fn>};

    template<typename Hook>
    static inline LoopStats loop{};

    template<typename Hook,
             typename E,
             typename F>
    [[gnu::always_inline]] static decltype(auto) entry(F&& f) {
        auto const t0 = TS::now();
        if constexpr(std::is_void_v<decltype(f())>) {
            f();
            record(stats<Hook, E>, TS::now() - t0, false);
        } else {
            auto const r = f();
            bool       busy{};
            if constexpr(std::is_same_v<std::remove_cvref_t<decltype(r)>, Kvasir::Turn>) {
                busy = r.isBusy();
            }
            record(stats<Hook, E>, TS::now() - t0, busy);
            return r;
        }
    }

    template<typename Hook,
             typename F>
    [[gnu::always_inline]] static decltype(auto) turn(F&& f) {
        auto const t0 = TS::now();
        if constexpr(std::is_void_v<decltype(f())>) {
            f();
            recordTurn(loop<Hook>, t0, TS::now());
        } else {
            auto const r = f();
            recordTurn(loop<Hook>, t0, TS::now());
            return r;
        }
    }

    /// The cost of one measurement (two reads), subtracted from every entry: the least of 16 back-to-back pairs.
    static void calibrate() {
        std::uint32_t least = 0xFFFF'FFFFU;
        for(int i = 0; i != 16; ++i) {
            auto const a = TS::now();
            auto const b = TS::now();
            least        = std::min(least, b - a);
        }
        overhead_ = least;
    }

    [[nodiscard]] static std::uint32_t overhead() { return overhead_; }

    [[nodiscard]] static EntryStats* first() { return head_; }

    /// Clear every entry and the loops (the report takes and clears).
    template<typename Hook>
    static void clear() {
        for(EntryStats* s = head_; s != nullptr; s = s->next) {
            s->count      = 0;
            s->worstTicks = 0;
            s->lastTicks  = 0;
            s->busyTurns  = 0;
            s->totalTicks = 0;
        }
        auto const last      = loop<Hook>.lastStart;
        auto const st        = loop<Hook>.started;
        loop<Hook>           = LoopStats{};
        loop<Hook>.lastStart = last;
        loop<Hook>.started   = st;
    }

private:
    [[gnu::noinline]] static void record(EntryStats&   s,
                                         std::uint32_t d,
                                         bool          busy) {
        d = d > overhead_ ? d - overhead_ : 0;
        if(!s.linked) {
            s.linked = true;
            s.next   = head_;
            head_    = &s;
        }
        ++s.count;
        s.totalTicks += d;
        s.lastTicks  = d;
        s.worstTicks = std::max(s.worstTicks, d);
        if(busy) { ++s.busyTurns; }
    }

    [[gnu::noinline]] static void recordTurn(LoopStats&    l,
                                             std::uint32_t start,
                                             std::uint32_t end) {
        ++l.turns;
        l.hookTicks += end - start;
        if(l.started) {
            auto const period = start - l.lastStart;
            l.periodTicks += period;
            l.worstPeriodTicks = std::max(l.worstPeriodTicks, period);
            if(budgetTicks != 0 && period > budgetTicks) { ++l.slips; }
        }
        l.lastStart = start;
        l.started   = true;
    }

    static inline EntryStats*   head_{};
    static inline std::uint32_t overhead_{};
};

/// A Startup-list entry on Hook::MainLoop: every Period the loop and each entry, one log line each, then cleared.
template<typename Config, auto const& Period, typename Hook = Kvasir::Hook::MainLoop>
struct LoopReport {
    using Profiler = HookProfiler<Config>;
    using TS       = typename Config::TimeSource;

    static Kvasir::Turn handler() {
        auto const now = TS::now();
        if(!started_) {
            Profiler::calibrate();
            started_ = true;
            last_    = now;
#ifdef USE_UC_LOG
            UC_LOG_I(
              "loop profiler: time source {} ({} Hz), overhead {} ticks per measurement, "
              "subtracted",
              TS::name,
              TS::ticksPerSecond,
              Profiler::overhead());
#endif
            return Kvasir::Turn::within(Period);
        }
        auto constexpr periodTicks
          = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(Period).count())
          * TS::ticksPerSecond / 1'000'000ULL;
        auto const since = static_cast<std::uint64_t>(now - last_);
        if(since < periodTicks) {
            return Kvasir::Turn::within(std::chrono::microseconds{static_cast<std::int64_t>(
              (periodTicks - since) * 1'000'000ULL / TS::ticksPerSecond)});
        }
        last_ = now;
        report();
        Profiler::template clear<Hook>();
        return Kvasir::Turn::within(Period);
    }

    /// The loop and every entry seen since the last report.
    static void report() {
#ifdef USE_UC_LOG
        auto const& l  = Profiler::template loop<Hook>;
        auto const  us = [](std::uint64_t t) {
            return std::chrono::microseconds{
              static_cast<std::int64_t>(t * 1'000'000ULL / TS::ticksPerSecond)};
        };
        auto const busyPermille
          = l.periodTicks == 0
            ? 0U
            : static_cast<std::uint32_t>(
                std::min<std::uint64_t>(1000, l.hookTicks * 1000 / l.periodTicks));
        UC_LOG_I("loop: {} turns, period avg {} worst {}, {} slip(s), hook busy {}.{}%",
                 l.turns,
                 us(l.turns > 1 ? l.periodTicks / (l.turns - 1) : 0),
                 us(l.worstPeriodTicks),
                 l.slips,
                 busyPermille / 10,
                 busyPermille % 10);
        for(EntryStats const* s = Profiler::first(); s != nullptr; s = s->next) {
            if(s->count == 0) { continue; }
            UC_LOG_I("loop:   {} {} runs avg {} worst {} busy {}",
                     s->name,
                     s->count,
                     us(s->totalTicks / s->count),
                     us(s->worstTicks),
                     s->busyTurns);
        }
#endif
    }

    using Extends = Startup::Extend<Kvasir::Hook::MainLoop, &LoopReport::handler>;

private:
    static inline std::uint32_t last_{};
    static inline bool          started_{};
};
}   // namespace Kvasir::Profile
