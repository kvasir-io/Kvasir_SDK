// Kvasir::BootGuard::Guard as a whole Startup entry (Runtime/BootGuard.hpp): runtimeInit reads the panic and fault
// records and the reset kind WITHOUT taking them, judges the previous run, stores the count, and runs the injected
// policy at the limit; turn() marks the run healthy after healthyAfter; plannedReset() makes the next watchdog reset
// no failure. The chip package's Kvasir::PM::resetKind() is a fake declared before the include (the guard's call is
// not dependent, so it must be declared first, as a chip header is in a firmware).
#include "kvasir/Port/ResetKind.hpp"

namespace Kvasir::PM {
inline ResetKind fakeReset = ResetKind::powerOn;

inline ResetKind resetKind() { return fakeReset; }
}   // namespace Kvasir::PM

#include "kvasir/Runtime/BootGuard.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace BG = Kvasir::BootGuard;
using Kvasir::ResetKind;
using namespace std::chrono_literals;
using Clock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

namespace {
struct Config {
    static constexpr std::uint16_t limit        = 2;
    static constexpr auto          healthyAfter = 30s;
};

using Guard = BG::Guard<Config, Clock>;

// A Config that keeps the state outside SRAM too (an external watchdog on the reset pin of a chip whose reset pin
// powers SRAM down): restore() trusts its copy only after a reset through the pin.
struct Mirror {
    static inline std::optional<BG::State> copy{};
    static inline int                      restores = 0;
    static inline int                      mirrors  = 0;
    static inline ResetKind                askedWith{};
};

struct MirroredConfig {
    static constexpr std::uint16_t limit         = 3;
    static constexpr bool          countExternal = true;

    static std::optional<BG::State> restore(ResetKind kind) {
        ++Mirror::restores;
        Mirror::askedWith = kind;
        return kind == ResetKind::external ? Mirror::copy : std::nullopt;
    }

    static void mirror(BG::State const& st) {
        ++Mirror::mirrors;
        Mirror::copy = st;
    }
};

using MirroredGuard = BG::Guard<MirroredConfig, Clock>;
static_assert(MirroredGuard::mirrored && !Guard::mirrored);
static_assert(MirroredGuard::rules.countExternal && !Guard::rules.countExternal);

// half a pair is refused (the guard's static_assert), not silently unmirrored
struct OnlyMirror {
    static void mirror(BG::State const&) {}
};

struct OnlyRestore {
    static std::optional<BG::State> restore(ResetKind) { return std::nullopt; }
};

struct WrongRestore {
    static BG::State restore(ResetKind) { return {}; }

    static void mirror(BG::State const&) {}
};

static_assert(BG::HalfMirroredConfig<OnlyMirror> && BG::HalfMirroredConfig<OnlyRestore>
              && BG::HalfMirroredConfig<WrongRestore>);
static_assert(!BG::HalfMirroredConfig<Config> && !BG::HalfMirroredConfig<MirroredConfig>);

// a boot after a reset that lost SRAM
constexpr BG::State inARow(std::uint16_t consecutive) {
    return BG::State{.bootCount   = 1,
                     .consecutive = consecutive,
                     .total       = consecutive,
                     .lastFailure = 0,
                     .lastDetail  = 0,
                     .flags       = 0,
                     .reserved    = 0};
}

void bootLosingSram(ResetKind kind) {
    BG::state.clear();
    Kvasir::PM::fakeReset = kind;
    MirroredGuard::runtimeInit();
}

void bootKeepingSram(ResetKind kind) {
    Kvasir::PM::fakeReset = kind;
    MirroredGuard::runtimeInit();
}

// a fresh boot: no records, no state
void powerOn() {
    BG::state.clear();
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Fault::lastFault.clear();
    Kvasir::PM::fakeReset = ResetKind::powerOn;
    Guard::runtimeInit();
}

void boot(ResetKind kind) {
    Kvasir::PM::fakeReset = kind;
    Guard::runtimeInit();
}

void leaveFault() {
    Kvasir::Fault::lastFault.update([](Kvasir::Fault::FullRecord& f) {
        f.count = 1;
        f.pc    = 0x1000'0100U;
    });
}
}   // namespace

template<>
struct Kvasir::BootGuard::LimitReaction<Kvasir::Tag::User> {
    using type = Kvasir::BootGuard::OnLimit::SafeMode;
};

int main() {
    // the image's format, after its specialisation: what BootGuard and the boot report read
    using Records = Kvasir::CrashRecord::Records<struct RecordsAnchor>;
    using Kvasir::Test::test;

    test("a power-on: fresh, nothing counted");
    powerOn();
    CHECK(Guard::report().verdict == BG::Verdict::fresh);
    CHECK(Guard::report().state.consecutive == 0U);
    CHECK(!Guard::inSafeMode());

    test("a fault record and a watchdog reset count once; the guard does not take the record");
    leaveFault();
    boot(ResetKind::watchdogTimeout);
    CHECK(Guard::report().verdict == BG::Verdict::failure);
    CHECK(Guard::report().state.consecutive == 1U);
    CHECK(Guard::report().state.lastFailure == static_cast<std::uint8_t>(BG::Failure::fault));
    CHECK(Records::Fault::recorded());                   // still there for the boot report
    auto const taken = Kvasir::Fault::takeLastFault();   // what the boot report does
    CHECK(taken && taken->pc == 0x1000'0100U);

    test(
      "a panic record and a software reset reach the limit: the reaction (OnLimit::SafeMode) runs");
    Kvasir::Panic::record(
      Kvasir::Panic::Info{.cause = Kvasir::Panic::Cause::abort, .pc = 0x1000'0200U});
    boot(ResetKind::softwareRequest);
    CHECK(Guard::report().verdict == BG::Verdict::failure);
    CHECK(Guard::report().state.consecutive == 2U);
    CHECK(Guard::report().atLimit());
    CHECK(Guard::inSafeMode());
    CHECK(Records::Panic::recorded());
    CHECK(Kvasir::Panic::takeLastPanic().has_value());
    Guard::leaveSafeMode();
    CHECK(!Guard::inSafeMode());

    test("healthyAfter: turn() after 30 s marks the run healthy, the next count starts at 0");
    Clock::reset();
    boot(ResetKind::softwareRequest);   // no records: neutral, the count stays
    CHECK(Guard::report().verdict == BG::Verdict::neutral);
    Clock::advance(29s);
    Guard::turn();
    auto const before = BG::state.load();
    CHECK(before && before->consecutive == 2U);
    Clock::advance(2s);
    Guard::turn();
    auto const after = BG::state.load();
    CHECK(after && after->consecutive == 0U);

    test("plannedReset(): the watchdog reset after it is no failure");
    Guard::plannedReset();
    boot(ResetKind::watchdogTimeout);
    CHECK(Guard::report().verdict == BG::Verdict::planned);
    CHECK(Guard::report().state.consecutive == 0U);

    test("a watchdog reset without a record and without a plan is a hang");
    boot(ResetKind::watchdogTimeout);
    CHECK(Guard::report().verdict == BG::Verdict::failure);
    CHECK(Guard::report().state.lastFailure == static_cast<std::uint8_t>(BG::Failure::watchdog));

    test("quietBoot: debug without failed runs, a warning with them or a halt");
    CHECK(Guard::quietBoot(BG::Report{BG::Verdict::fresh, {}, 2}));
    CHECK(Guard::quietBoot(BG::Report{BG::Verdict::planned, {}, 2}));
    CHECK(!Guard::quietBoot(BG::Report{BG::Verdict::failure, inARow(1), 2}));
    CHECK(!Guard::quietBoot(BG::Report{BG::Verdict::neutral, inARow(1), 2}));
    CHECK(!Guard::quietBoot(BG::Report{BG::Verdict::stillHalted, {}, 2}));

    test("mirrored: a fresh start is mirrored, restore() is not asked while SRAM holds the record");
    BG::state.clear();
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Fault::lastFault.clear();
    Mirror::copy.reset();
    bootLosingSram(ResetKind::powerOn);
    CHECK(MirroredGuard::report().verdict == BG::Verdict::fresh);
    CHECK(Mirror::restores == 1 && Mirror::askedWith == ResetKind::powerOn);
    CHECK(Mirror::copy && Mirror::copy->bootCount == 1U);
    bootKeepingSram(ResetKind::softwareRequest);
    CHECK(Mirror::restores == 1);   // SRAM was valid
    CHECK(MirroredGuard::report().verdict == BG::Verdict::neutral);
    CHECK(Mirror::copy && Mirror::copy->bootCount == 2U);

    test(
      "mirrored + countExternal: three hung runs ended by the reset pin reach the limit with SRAM "
      "lost each time");
    bootLosingSram(ResetKind::external);
    CHECK(MirroredGuard::report().verdict == BG::Verdict::failure);
    CHECK(MirroredGuard::report().state.lastFailure
          == static_cast<std::uint8_t>(BG::Failure::external));
    CHECK(MirroredGuard::report().state.consecutive == 1U
          && MirroredGuard::report().state.bootCount == 3U);
    bootLosingSram(ResetKind::external);
    bootLosingSram(ResetKind::external);
    CHECK(MirroredGuard::report().state.consecutive == 3U);
    CHECK(MirroredGuard::report().atLimit());
    CHECK(MirroredGuard::inSafeMode());   // the reaction (SafeMode) ran

    test("mirrored: markHealthy(), leaveSafeMode() and plannedReset() reach the copy");
    MirroredGuard::leaveSafeMode();
    CHECK(Mirror::copy && (Mirror::copy->flags & BG::safeMode) == 0);
    MirroredGuard::markHealthy();
    CHECK(Mirror::copy && Mirror::copy->consecutive == 0U);
    MirroredGuard::plannedReset();
    CHECK(Mirror::copy && (Mirror::copy->flags & BG::plannedResetPending) != 0);
    bootLosingSram(ResetKind::external);   // the planned reboot went through the pin: still planned
    CHECK(MirroredGuard::report().verdict == BG::Verdict::planned);

    test(
      "mirrored: a reset the copy cannot explain (a boot-ROM update's watchdog reboot) is a fresh "
      "start");
    auto const mirrorsBefore = Mirror::mirrors;
    bootLosingSram(ResetKind::watchdogTimeout);
    CHECK(Mirror::askedWith == ResetKind::watchdogTimeout);
    CHECK(MirroredGuard::report().verdict == BG::Verdict::fresh);
    CHECK(Mirror::mirrors == mirrorsBefore + 1);

    return Kvasir::Test::report();
}
