#pragma once
// The pure part of the boot-loop guard (Runtime/BootGuard.hpp): its persistent state, the verdict table and the
// policies. No registers, no chip header: the host tests take it directly.
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/ResetKind.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/StartUp/SafeState.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Kvasir::BootGuard {
// starvation: a health check starved and the watchdog bit (a Health "KHLT" record, or "KHRV" naming the onStarved
// a reset cut short); the detail is the check's index. external: a reset through the reset pin where an external
// watchdog drives it (Rules::countExternal). The record holds the number; uc_log's printer names it
// (printer/TargetRecords.hpp guardFailures, pinned per value by tests/records_cross_tests.cpp).
enum class Failure : std::uint8_t { none, panic, fault, watchdog, starvation, external };

// Bits OR-ed into State::flags (a byte); uc_log's printer names them (records_cross_tests.cpp pins each bit).
// A plain enum: bit flags of a byte field.
enum Flag : std::uint8_t {
    runOpen             = 1,   // the run started and was not closed by plannedReset()
    plannedResetPending = 2,   // plannedReset() was called: the next boot does not count this run
    halted              = 4,   // a Halt policy ran
    safeMode            = 8,   // the SafeMode policy ran; cleared by leaveSafeMode()
    healthyThisRun      = 16,
};

// Persistent "KBTG", 12 bytes.
struct State {
    std::uint32_t bootCount;     // boots since the state was created
    std::uint16_t consecutive;   // failed runs since the last healthy one
    std::uint16_t total;         // failed runs in all (saturating)
    std::uint8_t  lastFailure;   // Failure
    std::uint8_t  lastDetail;    // the Panic::Cause of a panic
    std::uint8_t  flags;         // Flag
    std::uint8_t  reserved;
};

struct Tag {
    static constexpr std::uint32_t id = persistentId("KBTG");
};

[[KVASIR_NOINIT]] inline Persistent<State, Tag> state;

struct Evidence {
    bool                         panic;
    std::optional<std::uint32_t> panicCause;
    bool                         fault;
    ResetKind                    reset;
    // the check a health supervisor's record names (Health::starvedCheck()); empty without one
    std::optional<std::uint32_t> starvedCheck{};
};

enum class Verdict : std::uint8_t { fresh, neutral, planned, failure, stillHalted };

[[nodiscard]] constexpr std::string_view name(Verdict v) {
    switch(v) {
    case Verdict::fresh:       return "fresh";
    case Verdict::neutral:     return "neutral";
    case Verdict::planned:     return "planned";
    case Verdict::failure:     return "failure";
    case Verdict::stillHalted: return "still halted";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view name(Failure f) {
    switch(f) {
    case Failure::none:       return "none";
    case Failure::panic:      return "panic";
    case Failure::fault:      return "fault";
    case Failure::watchdog:   return "watchdog, no record";
    case Failure::starvation: return "health check starved";
    case Failure::external:   return "external reset (reset pin)";
    }
    return "?";
}

struct Rules {
    bool countWatchdogWithoutRecord = true;
    bool countStarvation = true;   // a watchdog reset with a Health record is a failed run
    bool resetOnPowerOn  = true;
    bool resetOnExternal = false;
    // the reset pin is driven by an external watchdog: a reset through it ends a hung run, a failure like a watchdog
    // timeout (a planned reset still wins; beats resetOnExternal)
    bool countExternal = false;
};

/// What the run before this one was, and the state after it; pure, host-tested. The checks below run in priority
/// order (a panic of cause bootLoop is the guard's own and does not count again).
[[nodiscard]] constexpr std::pair<Verdict,
                                  State>
judge(std::optional<State> const& previous,
      Evidence const&             e,
      Rules const&                r) {
    auto const fail = [](State s, Failure f, std::uint32_t detail) {
        s.consecutive = s.consecutive == 0xFFFFU ? s.consecutive
                                                 : static_cast<std::uint16_t>(s.consecutive + 1);
        s.total       = s.total == 0xFFFFU ? s.total : static_cast<std::uint16_t>(s.total + 1);
        s.lastFailure = static_cast<std::uint8_t>(f);
        s.lastDetail  = static_cast<std::uint8_t>(detail);
        return std::pair{Verdict::failure, s};
    };
    if(!previous) { return {Verdict::fresh, State{}}; }
    State s = *previous;
    if(r.resetOnPowerOn && (e.reset == ResetKind::powerOn || e.reset == ResetKind::brownOut)) {
        s.consecutive = 0;
        s.flags       = 0;
        return {Verdict::fresh, s};
    }
    if((s.flags & halted) != 0) { return {Verdict::stillHalted, s}; }
    constexpr auto bootLoopCause = static_cast<std::uint32_t>(Panic::Cause::bootLoop);
    if(e.panic && e.panicCause == bootLoopCause) { return {Verdict::neutral, s}; }
    if(e.panic) { return fail(s, Failure::panic, e.panicCause.value_or(0)); }
    if(e.fault) { return fail(s, Failure::fault, 0); }
    // the supervisor stopped feeding (or the watchdog bit inside a check's onStarved): a record, so it counts even
    // where a watchdog reset without one does not. A record next to another reset (OnStarvation::Log, then a debugger
    // reset) is no failure: the supervisor did not end that run.
    if(r.countStarvation && e.starvedCheck && e.reset == ResetKind::watchdogTimeout) {
        return fail(s, Failure::starvation, *e.starvedCheck);
    }
    if((s.flags & plannedResetPending) != 0) { return {Verdict::planned, s}; }
    if(e.reset == ResetKind::watchdogTimeout && r.countWatchdogWithoutRecord) {
        return fail(s, Failure::watchdog, 0);
    }
    // No record says why: the run hung and the external watchdog pulled the pin. Where the reset loses SRAM (the
    // RP2350) the previous state is there only because Config::restore() kept it outside the chip.
    if(r.countExternal && e.reset == ResetKind::external) { return fail(s, Failure::external, 0); }
    if(r.resetOnExternal && e.reset == ResetKind::external) {
        s.consecutive = 0;
        return {Verdict::fresh, s};
    }
    // softwareRequest, debugger, watchdogForced, unknown and lowPower end here. lowPower counts like a software
    // request, not like a power-on: the application itself entered Standby or Stop, and the option bytes turned that
    // into a reset - the supply never went away (RM0430 6.1.1), so the count is neither reset nor raised. A run that
    // panicked or faulted first was counted above, whatever reset followed.
    return {Verdict::neutral, s};
}

struct Report {
    Verdict       verdict;
    State         state;
    std::uint16_t limit;

    [[nodiscard]] constexpr bool atLimit() const { return state.consecutive >= limit; }
};

// the reactions at the limit (BootGuard.hpp's header)
namespace OnLimit {
    struct Log {
        static void operator()(Report const&) {}
    };

    struct SafeMode {
        static void operator()(Report const&) {
            state.update([](State& s) { s.flags = static_cast<std::uint8_t>(s.flags | safeMode); });
        }
    };

    struct Panic {
        [[noreturn]] static void operator()(Report const& r) {
            ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::bootLoop, 0, r.state.consecutive);
        }
    };

    // The application's function, `void fn(Report const&)`, from the guard's runtimeInit (before main(), the log
    // backend up, the drivers after the guard not yet initialised).
    template<auto Fn>
    struct Call {
        static void operator()(Report const& r) { Fn(r); }
    };

    // Stop says what stops the watchdog: void (no watchdog runs), a watchdog whose disarm() takes no key, or - for a
    // gated watchdog (Config::gatedFeed, whose disarm() takes the supervisor's FeedKey) - the Health::Supervisor that
    // owns it: its holdOff() is the keyed disarm, allowed here because the guard's runtimeInit runs before the
    // supervisor's first service().
    template<typename Startup, typename Stop = void>
    struct Halt {
        [[noreturn]] static void operator()(Report const&) {
            state.update([](State& s) { s.flags = static_cast<std::uint8_t>(s.flags | halted); });
            Kvasir::Startup::SafeStateOf<Startup>{}();   // both cores' Hook::SafeState entries
            stopWatchdog();
            while(true) {
#ifdef __arm__
                asm volatile("wfi");
#endif
            }
        }

        // the watchdog part alone (the host test calls it; operator() never returns)
        static void stopWatchdog() {
            if constexpr(requires { Stop::canHoldOff; }) {
                static_assert(
                  Stop::canHoldOff,
                  "BootGuard::OnLimit::Halt: this supervisor's watchdog cannot be stopped "
                  "(an STM32 IWDG), so a halt would end in a watchdog reset - use "
                  "OnLimit::SafeMode or OnLimit::Panic");
                if constexpr(Stop::canHoldOff) { Stop::holdOff(); }
            } else if constexpr(requires { Stop::disarm(); }) {
                Stop::disarm();
            } else {
                static_assert(
                  std::is_void_v<Stop>,
                  "BootGuard::OnLimit::Halt<Startup, W>: W::disarm() takes the health supervisor's "
                  "FeedKey (a gated watchdog) - pass the Health::Supervisor instead: "
                  "OnLimit::Halt<Startup, Supervisor>");
            }
        }
    };

    // The guard writes its own line at the limit whatever the reaction ("... limit N: the policy runs"), so Log is that
    // line and Ignore is the same run without any word of its own.
    struct Ignore {
        static void operator()(Report const&) {}
    };

    // Whether this run started at the limit: `limits` is 1 then, 0 otherwise (RAM, this run; the failed runs themselves
    // the guard keeps in its record, BootGuard::state: consecutive and total).
    struct Count {
        static inline std::uint32_t volatile limits{};

        static void operator()(Report const&) { limits = limits + 1; }
    };

    // "In a row" for the guard: boots that start at the limit. The first Limit-1 of them go on (the guard's line);
    // the Limit-th - consecutive failed runs = limit + Limit - 1 - is Panic (Panic::Cause::bootLoop, the count as the
    // detail). Its own bootLoop panic is not counted again, so the next boot is at the same count and panics again:
    // the application's PanicReaction decides how that ends (as for OnLimit::Panic).
    template<std::uint16_t Limit>
    struct PanicAfter {
        static_assert(Limit >= 1,
                      "PanicAfter<0> would never let a boot reach the limit: use OnLimit::Panic");
        static constexpr std::uint16_t limit = Limit;

        static void operator()(Report const& r) {
            if(r.state.consecutive >= r.limit + Limit - 1U) {
                ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::bootLoop, 0, r.state.consecutive);
            }
        }
    };
}   // namespace OnLimit

// The image's reaction at the limit, a late hook (the table in kvasir/Port/Hooks.hpp): type, one of OnLimit:: or
// the application's own (`static void operator()(Report const&)`).
template<typename T, typename... Ts>
struct LimitReaction {
    using type = OnLimit::Log;
};

}   // namespace Kvasir::BootGuard
