// Multicore/ClockSync.hpp on two std::threads (core 0: primarySync, core 1: secondarySync), over two clocks that read
// one shared virtual time (no real clock): every read moves it one tick, the target's raw counter is the time minus a
// fixed offset - two counters started at different instants. A completed rendezvous makes the target read the
// reference's epoch to within the winning bracket (uncertainty(), half of it); a core that never arrives, or never
// stamps, ends the other side's wait at its timeout (both false, nothing synchronised, no syncTo).
#include "kvasir/Multicore/ClockSync.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ratio>
#include <thread>
#include <type_traits>

namespace {
// one virtual time for both clocks: a tick per read, from either thread
std::atomic<std::int64_t> virtualTime{0};

constexpr std::int64_t targetOffset
  = 123'456;   // the target counter started this many ticks after the reference

// The tick length decides how many reads a timeout takes: microseconds for the timeout scenarios (2 s = 2'000'000
// reads), femtoseconds for the one that must complete - its spinning thread would otherwise time out alone while the
// other one waits for a CPU (seen under a loaded CI host); 2 s of femtoseconds is never reached.
template<typename Period>
struct ReferenceClockT {
    using duration   = std::chrono::duration<std::int64_t, Period>;
    using rep        = duration::rep;
    using time_point = std::chrono::time_point<ReferenceClockT, duration>;

    static time_point now() { return time_point{duration{virtualTime.fetch_add(1) + 1}}; }
};

// One target type per scenario: ClockSync's state is per instantiation.
using ReferenceClock = ReferenceClockT<std::micro>;

template<int Scenario, typename Period = std::micro>
struct TargetClock {
    using duration = std::chrono::duration<std::int64_t, Period>;
    using rep      = duration::rep;

    static inline std::atomic<bool> synced{false};
    // an atomic: clang 23 calls a plain static of a class template in an unnamed namespace "set but not used" in
    // the instantiations that never read it (kvasir-build-toolchain)
    static inline std::atomic<rep> epoch{};

    static duration raw() { return duration{virtualTime.fetch_add(1) + 1 - targetOffset}; }

    static void syncTo(duration referenceAt,
                       duration rawAtSync) {
        epoch.store(referenceAt.count() - rawAtSync.count());
        synced.store(true);
    }
};

void completes() {
    Kvasir::Test::test("both cores: the target takes the reference's epoch to within the bracket");
    using Target = TargetClock<0, std::femto>;
    using Sync   = Kvasir::Multicore::ClockSync<ReferenceClockT<std::femto>, Target>;
    Sync::primaryPrepare();
    bool        primaryOk = false;
    std::thread core1{[&] { CHECK(Sync::secondarySync()); }};
    primaryOk = Sync::primarySync();
    core1.join();
    CHECK(primaryOk);
    CHECK(Sync::synchronised());
    CHECK(Target::synced.load());
    // the target counter is the reference minus targetOffset: the adopted epoch is that offset, off by at most half
    // the winning bracket (one tick of rounding: refMid = t0 + width / 2)
    auto const error = Target::epoch.load() - targetOffset;
    CHECK(Sync::uncertainty() >= 1);   // a bracket holds at least core 1's read between t0 and t1
    CHECK(error <= Sync::uncertainty() + 1 && error >= -(Sync::uncertainty() + 1));
}

void secondaryNeverArrives() {
    Kvasir::Test::test(
      "core 1 never arrives: primarySync gives up at its 2 s on the reference clock");
    using Sync = Kvasir::Multicore::ClockSync<ReferenceClock, TargetClock<1>>;
    Sync::primaryPrepare();
    auto const start = virtualTime.load();
    CHECK(!Sync::primarySync());
    CHECK(!Sync::synchronised());
    // the deadline: 2 s of microsecond ticks, one tick per read
    CHECK(virtualTime.load() - start >= 2'000'000);
    CHECK(!TargetClock<1>::synced.load());
}

void primaryNeverStamps() {
    Kvasir::Test::test(
      "core 0 never stamps: secondarySync gives up at its 2 s on the target's raw counter");
    using Sync = Kvasir::Multicore::ClockSync<ReferenceClock, TargetClock<2>>;
    Sync::primaryPrepare();
    auto const start = virtualTime.load();
    CHECK(!Sync::secondarySync());
    CHECK(!Sync::synchronised());
    CHECK(virtualTime.load() - start >= 2'000'000);
    CHECK(!TargetClock<2>::synced.load());
}

// the bracket's start value -1 needs a signed tick type (ClockSync.hpp static_assert)
static_assert(std::is_signed_v<ReferenceClock::rep>
              && std::is_signed_v<ReferenceClockT<std::femto>::rep>);
}   // namespace

int main() {
    completes();
    secondaryNeverArrives();
    primaryNeverStamps();
    return Kvasir::Test::report();
}
