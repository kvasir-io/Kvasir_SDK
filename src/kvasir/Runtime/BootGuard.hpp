#pragma once
// A boot-loop guard: counts the runs in a row that ended in a panic, a fault or a watchdog hang before they became
// healthy, across resets, and does what the application chose when the count reaches its limit.
//
//     struct GuardConfig {
//         static constexpr std::uint16_t limit = 3;                  // failed runs in a row before the policy
//         static constexpr auto healthyAfter   = std::chrono::seconds{30};   // a run this long is healthy
//     };
//     using Guard = Kvasir::BootGuard::Guard<GuardConfig, HW::SystickClock>;
//     using Startup = Kvasir::Startup::Startup<..., HW::ComBackend, Guard, Kvasir::Boot::Report<>, /* drivers */ ...>;
//     template<>
//     struct Kvasir::BootGuard::LimitReaction<Kvasir::Tag::User> {
//         using type = Kvasir::BootGuard::OnLimit::SafeMode;
//     };
//
// The guard reads the panic/fault records and PM::resetKind() WITHOUT taking them, so Boot::Report still reports
// them; its own count lives in a CRC-checked Persistent record ("KBTG"). It needs the records (FaultRecord.hpp), not
// the fault handler; PM::resetKind() comes from the chip package, declared before this header.
//
// Config knobs, each optional except `limit`: healthyAfter (0: only Guard::markHealthy()),
// countWatchdogWithoutRecord (true: a watchdog timeout with no record and no planned reset is a hang),
// countStarvation (true: a watchdog timeout with a health supervisor's starvation record, or with a reset inside a
// check's onStarved, is a failed run of its own kind, "health check starved", the check's index as the detail),
// resetOnPowerOn (true), resetOnExternal (false), countExternal (false: true where the reset pin is driven by an
// external watchdog, so a reset through it ends a hung run - "external reset", a failure like a watchdog timeout;
// a planned reset still wins, and it beats resetOnExternal). A reset of the cores alone (a debugger's reset or flash)
// is the chip's softwareRequest on every chip, also right after a watchdog reset (the RP chips tell it apart at their
// first init step, chip_rp_common ResetCauseRules.hpp): it never counts.
//
// Keeping the state outside SRAM. The record lives in SRAM, and some resets lose it: on the RP2350 the reset pin, a
// power-on, a brown-out or the rescue reset power SRAM down (they reset the power manager), so the very reset an
// external watchdog makes leaves no record to count. A Config that can keep a copy elsewhere (NOR flash, FRAM, an
// external EEPROM) provides BOTH of
//     static std::optional<Kvasir::BootGuard::State> restore(Kvasir::ResetKind);
//     static void mirror(Kvasir::BootGuard::State const&);
// (one without the other is a compile error). The semantics:
//   - restore() is asked only when the SRAM record is not valid, in runtimeInit, with the reset kind; what it returns
//     is judged as if SRAM had kept it (nullopt: a fresh start).
//   - mirror() gets the state after every change the guard itself makes: the boot's judgement (before the limit's
//     reaction runs: a Halt never returns), markHealthy(), plannedReset() and leaveSafeMode(). It runs in thread
//     mode, before main() for the first one; a slow store (a flash erase) delays the boot by that much. The flags
//     a reaction sets (halted, safeMode) are not mirrored: after a reset that lost SRAM the count alone brings the
//     reaction back.
//   - Every reset that keeps SRAM (a panic, a fault, the chip's own watchdog, a software reboot) leaves the SRAM
//     record, which is always newer than or equal to the mirror; restore() is not asked then.
//   - restore() decides by the reset kind whether to trust its copy. A copy is stale after a reset the guard never
//     heard of that also loses SRAM: on the RP2350 an update through the boot ROM (BOOTSEL, USB) loses SRAM and its
//     reboot is a watchdog timeout - a copy restored then would count the update as a hang. Restore only for the
//     reset that needs it (ResetKind::external where the pin is the watchdog), nullopt otherwise.
// Reactions at the limit, the image's choice through the late hook LimitReaction (the table in kvasir/Port/Hooks.hpp):
// OnLimit::Log (the default: the guard's log line is the action), OnLimit::SafeMode (a flag the application asks
// with Guard::inSafeMode()), OnLimit::Panic (Panic::Cause::bootLoop, the count as the detail; a panic of that cause
// is not counted again), OnLimit::Halt<Startup, Stop> (Hook::SafeState, the watchdog stopped, wfi; Stop is
// void, a watchdog with a plain disarm(), or - for a gated watchdog - the Health::Supervisor that owns it),
// OnLimit::Call<&fn> (the application's `void fn(Report const&)`, from the guard's runtimeInit).
#include "kvasir/Health/HealthRecords.hpp"
#include "kvasir/Port/Check.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Runtime/BootGuardRules.hpp"
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Periodic.hpp"

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Kvasir::BootGuard {
// A Config that keeps the guard's state outside SRAM as well (the header comment): both members.
template<typename Config>
concept MirroredConfig = requires(State const& st) {
    { Config::restore(ResetKind{}) } -> std::same_as<std::optional<State>>;
    Config::mirror(st);
};

template<typename Config>
concept HalfMirroredConfig
  = !MirroredConfig<Config>
 && (requires { Config::restore(ResetKind{}); } || requires(State const& st) { Config::mirror(st); });

// the Startup-list entry
template<typename Config, PollClock Clock>
struct Guard {
    static_assert(
      Port::ChipResetKind<decltype(PM::resetKind())>,
      "Kvasir port: the chip package provides no PM::resetKind() (its PM.hpp / chip_reset.hpp) - "
      "BootGuard reads the reset kind");
    static constexpr std::uint16_t limit = Config::limit;
    static_assert(limit > 0,
                  "a boot guard needs a limit of at least one failed run");

    // each knob the Config's member if it has one, else Rules' default
    static constexpr Rules rules{
      .countWatchdogWithoutRecord
      = KVASIR_CONFIG_OR(Config, countWatchdogWithoutRecord, Rules{}.countWatchdogWithoutRecord),
      .countStarvation = KVASIR_CONFIG_OR(Config, countStarvation, Rules{}.countStarvation),
      .resetOnPowerOn  = KVASIR_CONFIG_OR(Config, resetOnPowerOn, Rules{}.resetOnPowerOn),
      .resetOnExternal = KVASIR_CONFIG_OR(Config, resetOnExternal, Rules{}.resetOnExternal),
      .countExternal   = KVASIR_CONFIG_OR(Config, countExternal, Rules{}.countExternal)};

    static constexpr bool mirrored = MirroredConfig<Config>;
    static_assert(!HalfMirroredConfig<Config>,
                  "BootGuard Config: restore(ResetKind) -> std::optional<State> and mirror(State "
                  "const&) come as a "
                  "pair - one of them is missing or has another signature");

    static constexpr bool healthyByTime
      = KVASIR_CONFIG_OR(Config, healthyAfter, std::chrono::seconds{0}).count() != 0;

    // List it after the ComBackend, before Boot::Report and any driver: its line needs the log, and the report takes.
    static void runtimeInit() {
        // the image's format, looked up through Config: dependent, so after the application's specialisation
        using R = CrashRecord::Records<Config>;
        Evidence const e{.panic        = R::Panic::recorded(),
                         .panicCause   = R::Panic::cause(),
                         .fault        = R::Fault::recorded(),
                         .reset        = PM::resetKind(),
                         .starvedCheck = [] -> std::optional<std::uint32_t> {
                             if constexpr(rules.countStarvation) {
                                 return Health::starvedCheck();
                             } else {
                                 return std::nullopt;
                             }
                         }()};
        auto           previous = state.load();
        if constexpr(mirrored) {
            if(!previous) { previous = Config::restore(e.reset); }
        }
        auto [verdict, s] = judge(previous, e, rules);
        s.flags           = static_cast<std::uint8_t>((s.flags | runOpen)
                                                      & ~(plannedResetPending | healthyThisRun));
        s.bootCount       = s.bootCount + 1;
        state.store(s);
        mirrorState();
        report_ = Report{verdict, s, limit};
        logReport();
        if(report_.atLimit() || verdict == Verdict::stillHalted) { applyPolicy<>(report_); }
        if constexpr(healthyByTime) { healthyAt_.restart(Config::healthyAfter); }
    }

    // a turn of the main loop: a run that lasted healthyAfter is healthy
    static void turn() {
        if constexpr(healthyByTime) {
            if(healthyAt_.due()) { markHealthy(); }
        }
    }

    using Extends = Startup::Extend<Hook::MainLoop, &turn>;

    /// This run is healthy: the count of failed runs in a row goes back to 0 (the total stays).
    static void markHealthy() {
        state.update([](State& st) {
            st.consecutive = 0;
            st.flags       = static_cast<std::uint8_t>(st.flags | healthyThisRun);
        });
        mirrorState();
    }

    /// Call right before a deliberate reset (an update, a reboot on request): the next boot does not count it.
    static void plannedReset() {
        state.update([](State& st) {
            st.flags = static_cast<std::uint8_t>((st.flags | plannedResetPending) & ~runOpen);
        });
        mirrorState();
    }

    [[nodiscard]] static bool inSafeMode() {
        auto const st = state.load();
        return st && (st->flags & safeMode) != 0;
    }

    static void leaveSafeMode() {
        state.update([](State& st) { st.flags = static_cast<std::uint8_t>(st.flags & ~safeMode); });
        mirrorState();
    }

    [[nodiscard]] static Report const& report() { return report_; }

    /// A boot with nothing failed before it (fresh, neutral or planned, none in a row) says nothing the boot line does
    /// not: debug. Failed runs in a row, or a halt carried over, are what the guard is there for: a warning.
    [[nodiscard]] static constexpr bool quietBoot(Report const& r) {
        return r.state.consecutive == 0
            && (r.verdict == Verdict::fresh || r.verdict == Verdict::neutral
                || r.verdict == Verdict::planned);
    }

    static void logReport() {
#ifdef USE_UC_LOG
        [[maybe_unused]] auto const& r = report_;
        if(quietBoot(r)) {
            UC_LOG_D("boot guard: {} (boot {}, {} failure(s) in all)",
                     name(r.verdict),
                     r.state.bootCount,
                     r.state.total);
            return;
        }
        UC_LOG_W(
          "boot guard: {}, {} failed run(s) in a row (last: {}), limit {}{} (boot {}, {} "
          "failure(s) in all)",
          name(r.verdict),
          r.state.consecutive,
          name(static_cast<Failure>(r.state.lastFailure)),
          r.limit,
          std::string_view{r.atLimit() ? ": the policy runs" : ""},
          r.state.bootCount,
          r.state.total);
#endif
    }

    // For the list rule GuardBeforeReport (StartUp/ListRules.hpp): the guard reads the records Boot::Report takes, so
    // it is listed before the report (after the ComBackend, as above; Runtime::Standard lists both in that order).
    static constexpr bool isBootGuard = true;

private:
    static void mirrorState() {
        if constexpr(mirrored) {
            if(auto const st = state.load()) { Config::mirror(*st); }
        }
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    static void applyPolicy(Report const& r) {
        typename ::Kvasir::Port::LateT<LimitReaction<::Kvasir::Tag::User>, Dummy...>::type{}(r);
    }

    static inline Report          report_{};
    static inline Deadline<Clock> healthyAt_{};
};
}   // namespace Kvasir::BootGuard
