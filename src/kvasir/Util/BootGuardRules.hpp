#pragma once
// The pure part of the boot-loop guard (Util/BootGuard.hpp): its persistent state, the verdict table and the
// policies. No registers, no chip header: the host tests take it directly.
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/Panic.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/ResetKind.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Kvasir::BootGuard {
enum class Failure : std::uint8_t { none, panic, fault, watchdog };

enum Flag : std::uint8_t {
    runOpen             = 1,   // the run started and was not closed by plannedReset()
    plannedResetPending = 2,   // plannedReset() was called: the next boot does not count this run
    halted              = 4,   // a Halt policy ran
    safeMode            = 8,   // the SafeMode policy ran; cleared by leaveSafeMode()
    healthyThisRun      = 16,
};

// Persistent "KBTG" version 1, 12 bytes.
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
    static constexpr std::uint32_t id      = persistentId("KBTG");
    static constexpr std::uint16_t version = 1;
};

[[gnu::section(".noInit")]] inline Persistent<State, Tag> state;

struct Evidence {
    bool                         panic;
    std::optional<std::uint32_t> panicCause;
    bool                         fault;
    ResetKind                    reset;
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
    case Failure::none:     return "none";
    case Failure::panic:    return "panic";
    case Failure::fault:    return "fault";
    case Failure::watchdog: return "watchdog, no record";
    }
    return "?";
}

struct Rules {
    bool countWatchdogWithoutRecord = true;
    bool resetOnPowerOn             = true;
    bool resetOnExternal            = false;
    bool rp2040StaleReason          = false;
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
    if((s.flags & plannedResetPending) != 0) { return {Verdict::planned, s}; }
    if(e.reset == ResetKind::watchdogTimeout && r.countWatchdogWithoutRecord
       && (!r.rp2040StaleReason || (s.flags & runOpen) != 0))
    {
        return fail(s, Failure::watchdog, 0);
    }
    if(r.resetOnExternal && e.reset == ResetKind::external) {
        s.consecutive = 0;
        return {Verdict::fresh, s};
    }
    return {Verdict::neutral, s};
}

struct Report {
    Verdict       verdict;
    State         state;
    std::uint16_t limit;

    [[nodiscard]] constexpr bool atLimit() const { return state.consecutive >= limit; }
};

// policies at the limit
struct LogAndContinue {
    static void operator()(Report const&) {}
};

struct SafeMode {
    static void operator()(Report const&) {
        state.update([](State& s) { s.flags = static_cast<std::uint8_t>(s.flags | safeMode); });
    }
};

struct RaisePanic {
    [[noreturn]] static void operator()(Report const& r) {
        Panic::raiseAt(Panic::Cause::bootLoop, 0, r.state.consecutive);
    }
};

template<typename Startup, typename Watchdog = void>
struct Halt {
    [[noreturn]] static void operator()(Report const&) {
        state.update([](State& s) { s.flags = static_cast<std::uint8_t>(s.flags | halted); });
        Startup::template run<Hook::SafeState>();
        if constexpr(!std::is_void_v<Watchdog>) { Watchdog::disarm(); }
        while(true) {
#ifdef __arm__
            asm volatile("wfi");
#endif
        }
    }
};

template<typename...>
inline constexpr auto injectedPolicy = LogAndContinue{};

}   // namespace Kvasir::BootGuard
