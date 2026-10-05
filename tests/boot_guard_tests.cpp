// Kvasir::BootGuard::judge (Util/BootGuardRules.hpp): one case per row of the verdict table, and the
// combinations - a panic and a watchdog reset count once, a planned reset with a panic counts, power-on resets the
// count and keeps the total, a halt stays halted until power-on, the counters saturate, a bootLoop panic is not
// counted again, the RP2040 stale-REASON rule. All at compile time, plus the Persistent state round trip.
#include "kvasir/Util/BootGuardRules.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <optional>

namespace BG = Kvasir::BootGuard;
using BG::Verdict;
using Kvasir::ResetKind;

namespace {
constexpr BG::State running(std::uint16_t consecutive,
                            std::uint8_t  flags = BG::runOpen) {
    return BG::State{.bootCount   = 5,
                     .consecutive = consecutive,
                     .total       = 7,
                     .lastFailure = 0,
                     .lastDetail  = 0,
                     .flags       = flags,
                     .reserved    = 0};
}

constexpr BG::Evidence ev(ResetKind                    r,
                          bool                         panic = false,
                          bool                         fault = false,
                          std::optional<std::uint32_t> cause = std::nullopt) {
    return {.panic = panic, .panicCause = cause, .fault = fault, .reset = r};
}

constexpr BG::Rules rules{};
constexpr auto      assertCause = static_cast<std::uint32_t>(Kvasir::Panic::Cause::assertion);
constexpr auto      loopCause   = static_cast<std::uint32_t>(Kvasir::Panic::Cause::bootLoop);

// no valid state: fresh, all zero
static_assert(BG::judge(std::nullopt,
                        ev(ResetKind::watchdogTimeout,
                           true),
                        rules)
                .first
              == Verdict::fresh);
static_assert(BG::judge(std::nullopt,
                        ev(ResetKind::debugger),
                        rules)
                .second.consecutive
              == 0);

// power-on: fresh, count 0, total kept; with resetOnPowerOn = false a panic still counts
static_assert(BG::judge(running(2),
                        ev(ResetKind::powerOn,
                           true),
                        rules)
                .first
              == Verdict::fresh);
static_assert(BG::judge(running(2),
                        ev(ResetKind::powerOn,
                           true),
                        rules)
                .second.consecutive
              == 0);
static_assert(BG::judge(running(2),
                        ev(ResetKind::brownOut),
                        rules)
                .second.total
              == 7);
static_assert(BG::judge(running(2),
                        ev(ResetKind::powerOn,
                           true),
                        BG::Rules{.resetOnPowerOn = false})
                .first
              == Verdict::failure);

// a halt stays halted until power-on
static_assert(BG::judge(running(3,
                                BG::halted),
                        ev(ResetKind::watchdogTimeout,
                           true),
                        rules)
                .first
              == Verdict::stillHalted);
static_assert(BG::judge(running(3,
                                BG::halted),
                        ev(ResetKind::powerOn),
                        rules)
                .first
              == Verdict::fresh);

// a panic, a fault: failure, counted once even with a watchdog reset
static_assert(BG::judge(running(1),
                        ev(ResetKind::watchdogTimeout,
                           true,
                           false,
                           assertCause),
                        rules)
                .first
              == Verdict::failure);
static_assert(BG::judge(running(1),
                        ev(ResetKind::watchdogTimeout,
                           true,
                           false,
                           assertCause),
                        rules)
                .second.consecutive
              == 2);
static_assert(BG::judge(running(1),
                        ev(ResetKind::watchdogTimeout,
                           true,
                           false,
                           assertCause),
                        rules)
                .second.total
              == 8);
static_assert(BG::judge(running(0),
                        ev(ResetKind::softwareRequest,
                           false,
                           true),
                        rules)
                .second.lastFailure
              == static_cast<std::uint8_t>(BG::Failure::fault));

// the RaisePanic policy's own panic is not counted again
static_assert(BG::judge(running(3),
                        ev(ResetKind::watchdogTimeout,
                           true,
                           false,
                           loopCause),
                        rules)
                .first
              == Verdict::neutral);

// a planned reset: neutral; with a panic record the record wins
static_assert(BG::judge(running(1,
                                BG::plannedResetPending),
                        ev(ResetKind::watchdogTimeout),
                        rules)
                .first
              == Verdict::planned);
static_assert(BG::judge(running(1,
                                BG::plannedResetPending),
                        ev(ResetKind::watchdogTimeout,
                           true),
                        rules)
                .first
              == Verdict::failure);

// a watchdog timeout without a record is a hang, unless switched off
static_assert(BG::judge(running(0),
                        ev(ResetKind::watchdogTimeout),
                        rules)
                .first
              == Verdict::failure);
static_assert(BG::judge(running(0),
                        ev(ResetKind::watchdogTimeout),
                        rules)
                .second.lastFailure
              == static_cast<std::uint8_t>(BG::Failure::watchdog));
static_assert(BG::judge(running(0),
                        ev(ResetKind::watchdogTimeout),
                        BG::Rules{.countWatchdogWithoutRecord = false})
                .first
              == Verdict::neutral);
// ... and on the RP2040 only if the run was open (a debugger flash leaves REASON as it was)
static_assert(BG::judge(running(0,
                                0),
                        ev(ResetKind::watchdogTimeout),
                        BG::Rules{.rp2040StaleReason = true})
                .first
              == Verdict::neutral);
static_assert(BG::judge(running(0),
                        ev(ResetKind::watchdogTimeout),
                        BG::Rules{.rp2040StaleReason = true})
                .first
              == Verdict::failure);

// a forced watchdog, the debugger, a software request without a record: neutral
static_assert(BG::judge(running(2),
                        ev(ResetKind::watchdogForced),
                        rules)
                .first
              == Verdict::neutral);
static_assert(BG::judge(running(2),
                        ev(ResetKind::debugger),
                        rules)
                .second.consecutive
              == 2);

// the reset pin: neutral, or fresh with resetOnExternal
static_assert(BG::judge(running(2),
                        ev(ResetKind::external),
                        rules)
                .first
              == Verdict::neutral);
static_assert(BG::judge(running(2),
                        ev(ResetKind::external),
                        BG::Rules{.resetOnExternal = true})
                .second.consecutive
              == 0);

// saturation
constexpr BG::State saturated = [] {
    auto st  = running(0xFFFF);
    st.total = 0xFFFF;
    return st;
}();
static_assert(BG::judge(saturated,
                        ev(ResetKind::unknown,
                           true),
                        rules)
                .second.consecutive
              == 0xFFFF);
static_assert(BG::judge(saturated,
                        ev(ResetKind::unknown,
                           true),
                        rules)
                .second.total
              == 0xFFFF);

static_assert(BG::Report{Verdict::failure,
                         running(3),
                         3}
                .atLimit());
static_assert(!BG::Report{Verdict::failure,
                          running(2),
                          3}
                 .atLimit());
}   // namespace

int main() {
    using Kvasir::Test::test;
    test("the persistent state round trip, and the SafeMode policy's flag");
    BG::state.clear();
    CHECK(!BG::state.load());
    BG::state.store(running(2));
    BG::SafeMode{}(BG::Report{Verdict::failure, running(3), 3});
    auto const s = BG::state.load();
    CHECK(s && s->consecutive == 2 && (s->flags & BG::safeMode) != 0);
    return Kvasir::Test::report();
}
