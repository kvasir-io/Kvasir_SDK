#pragma once
// A boot-loop guard: counts the runs in a row that ended in a panic, a fault or a watchdog hang before they became
// healthy, across resets, and does what the application chose when the count reaches its limit.
//
//     struct GuardConfig {
//         static constexpr std::uint16_t limit = 3;                  // failed runs in a row before the policy
//         static constexpr auto healthyAfter   = std::chrono::seconds{30};   // a run this long is healthy
//     };
//     using Guard = Kvasir::BootGuard::Guard<GuardConfig, HW::SystickClock>;
//     using Startup = Kvasir::Startup::Startup<..., HW::ComBackend, Guard, /* drivers */ ...>;   // after the log
//     template<> inline constexpr auto Kvasir::BootGuard::injectedPolicy<> = Kvasir::BootGuard::SafeMode{};
//
// The guard reads the panic/fault records and PM::resetKind() WITHOUT taking them, so Boot::logBoot() still reports
// them; its own count lives in a CRC-checked Persistent record ("KBTG").
//
// Config knobs, each optional except `limit`: healthyAfter (0: only Guard::markHealthy()),
// countWatchdogWithoutRecord (true: a watchdog timeout with no record and no planned reset is a hang),
// resetOnPowerOn (true), resetOnExternal (false), rp2040StaleReason (false: a watchdog verdict also needs the
// previous run to have been open - the RP2040 keeps REASON across a debugger's reset).
//
// Policies at the limit: LogAndContinue (the default: the guard's log line is the action), SafeMode (a flag the
// application asks with Guard::inSafeMode()), RaisePanic (Panic::Cause::bootLoop, the count as the detail; a
// panic of that cause is not counted again), Halt<Startup, Watchdog> (Hook::SafeState, the watchdog disarmed, wfi).
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/BootGuardRules.hpp"
#include "kvasir/Util/FaultHandler.hpp"
#include "kvasir/Util/Panic.hpp"
#include "kvasir/Util/Periodic.hpp"

#include <cstdint>
#include <string_view>

namespace Kvasir::BootGuard {
// the Startup-list entry
template<typename Config, PollClock Clock>
struct Guard {
    static constexpr std::uint16_t limit = Config::limit;
    static_assert(limit > 0,
                  "a boot guard needs a limit of at least one failed run");

    static constexpr Rules rules = [] {
        Rules r{};
        if constexpr(requires { Config::countWatchdogWithoutRecord; }) {
            r.countWatchdogWithoutRecord = Config::countWatchdogWithoutRecord;
        }
        if constexpr(requires { Config::resetOnPowerOn; }) {
            r.resetOnPowerOn = Config::resetOnPowerOn;
        }
        if constexpr(requires { Config::resetOnExternal; }) {
            r.resetOnExternal = Config::resetOnExternal;
        }
        if constexpr(requires { Config::rp2040StaleReason; }) {
            r.rp2040StaleReason = Config::rp2040StaleReason;
        }
        return r;
    }();

    static constexpr bool healthyByTime = [] {
        if constexpr(requires { Config::healthyAfter; }) {
            return Config::healthyAfter.count() != 0;
        } else {
            return false;
        }
    }();

    // List it right after the ComBackend: its line needs the log, and it must run before any driver.
    static void runtimeInit() {
        Evidence const e{.panic      = Panic::Detail::panicRecorded<>(),
                         .panicCause = Panic::Detail::recordedCause<>(),
                         .fault      = Fault::Detail::faultRecorded<>(),
                         .reset      = PM::resetKind()};
        auto [verdict, s] = judge(state.load(), e, rules);
        s.flags           = static_cast<std::uint8_t>((s.flags | runOpen)
                                                      & ~(plannedResetPending | healthyThisRun));
        s.bootCount       = s.bootCount + 1;
        state.store(s);
        report_ = Report{verdict, s, limit};
        logReport();
        if(report_.atLimit() || verdict == Verdict::stillHalted) { applyPolicy<>(report_); }
        if constexpr(healthyByTime) { healthyAt_.restart(Config::healthyAfter); }
    }

    // a turn of the main loop: a run that lasted healthyAfter is healthy
    static void poll() {
        if constexpr(healthyByTime) {
            if(healthyAt_.due()) { markHealthy(); }
        }
    }

    using Extends = Startup::Extend<Hook::MainLoop, &poll>;

    /// This run is healthy: the count of failed runs in a row goes back to 0 (the total stays).
    static void markHealthy() {
        state.update([](State& st) {
            st.consecutive = 0;
            st.flags       = static_cast<std::uint8_t>(st.flags | healthyThisRun);
        });
    }

    /// Call right before a deliberate reset (an update, a reboot on request): the next boot does not count it.
    static void plannedReset() {
        state.update([](State& st) {
            st.flags = static_cast<std::uint8_t>((st.flags | plannedResetPending) & ~runOpen);
        });
    }

    [[nodiscard]] static bool inSafeMode() {
        auto const st = state.load();
        return st && (st->flags & safeMode) != 0;
    }

    static void leaveSafeMode() {
        state.update([](State& st) { st.flags = static_cast<std::uint8_t>(st.flags & ~safeMode); });
    }

    [[nodiscard]] static Report const& report() { return report_; }

    static void logReport() {
        [[maybe_unused]] auto const& r = report_;
        UC_LOG_I(
          "boot guard: {}, {} failed run(s) in a row (last: {}), limit {}{} (boot {}, {} "
          "failure(s) in all)",
          name(r.verdict),
          r.state.consecutive,
          name(static_cast<Failure>(r.state.lastFailure)),
          r.limit,
          std::string_view{r.atLimit() ? ": the policy runs" : ""},
          r.state.bootCount,
          r.state.total);
    }

private:
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    static void applyPolicy(Report const& r) {
        injectedPolicy<Dummy...>(r);
    }

    static inline Report          report_{};
    static inline Deadline<Clock> healthyAt_{};
};
}   // namespace Kvasir::BootGuard
