// Kvasir::Health::Supervisor::turn() and Kvasir::Health::Service<> (the supervisor as a Startup entry): turn() is
// service() with the time to the next tick for a sleeping loop; Service<> drives the supervisor the late hook
// SupervisorFor<Tag::User> names, from Hook::MainLoop, after the entries listed before it, and the loop's Turn is the
// earliest of all; its runtimeInit (the boot half) holds a stoppable watchdog off and reports the run before's
// starvation, once. The real Startup does not instantiate on the host: the list is driven through
// Detail::dispatch, which is all Startup::runTurn does.
#include "kvasir/Health/Health.hpp"

#include "fixtures/Health.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include "test_harness.hpp"

#include <chrono>

namespace H = Kvasir::Health;
using namespace std::chrono_literals;

namespace {
using namespace Kvasir::Test::HealthFixture;

template<typename Tag>
struct Beat {
    static constexpr std::string_view name        = "beat";
    static constexpr auto             maxInterval = 50ms;
};

// the entry that owns the check: beats every turn it is driven, wants the loop again in 30 ms
struct Producer {
    static inline int turns = 0;

    static Kvasir::Turn turn() {
        ++turns;
        H::checkIn<Beat<Producer>>();
        return Kvasir::Turn::within(30ms);
    }

    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Producer::turn>;
};

// the hand form's supervisor (turn() by hand) and the listed one (Service<>): separate tags, separate state
struct HandTag {};

using HandSup = Sup<HandTag, Beat<HandTag>>;

using Listed   = Sup<Producer, Beat<Producer>>;
using LoopList = brigand::list<Producer, H::Service<>>;

// a watchdog that can be stopped: Service<>'s runtimeInit holds it off (a supervisor of its own, bound to its tag)
template<typename Tag>
struct StoppableWatchdog {
    static constexpr auto              timeout = std::chrono::milliseconds{500};
    [[maybe_unused]] static inline int feeds   = 0;
    [[maybe_unused]] static inline int disarms = 0;

    static void feed() { ++feeds; }

    static void disarm() { ++disarms; }
};

struct HoldTag {};

using HoldSup = Kvasir::Health::
  Supervisor<Startup<HoldTag, Beat<HoldTag>>, StoppableWatchdog<HoldTag>, FakeClock, Period10>;

Kvasir::Turn loopTurn() {
    return Kvasir::Startup::Detail::dispatch<Kvasir::Hook::MainLoop, Kvasir::Turn>(
      static_cast<LoopList*>(nullptr));
}
}   // namespace

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::HealthFixture::TestPolicy;
};

template<>
struct Kvasir::Health::SupervisorFor<Kvasir::Tag::User> {
    using type = ::Listed;
};

template<>
struct Kvasir::Health::SupervisorFor<HoldTag> {
    using type = ::HoldSup;
};

namespace {
void turnIsService() {
    using Kvasir::Test::test;
    test(
      "turn(): the first call starts the supervisor, then service() and the time to the next tick");
    CHECK(HandSup::turn() == Kvasir::Turn::within(10ms));   // started: the first tick one period on
    int const fed = Watchdog<HandTag>::feeds;
    advance(4ms);
    CHECK(HandSup::turn()
          == Kvasir::Turn::within(6ms));   // between two ticks: the rest of the period
    CHECK(Watchdog<HandTag>::feeds == fed);
    advance(6ms);
    H::checkIn<Beat<HandTag>>();
    CHECK(HandSup::turn()
          == Kvasir::Turn::within(10ms));   // a tick: judged and fed, the next one a period on
    CHECK(Watchdog<HandTag>::feeds == fed + 1);
    test("turn(): a silent check starves as with service()");
    Calls const c{};
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        (void)HandSup::turn();
    }
    CHECK(c.count() == 1 && c.last().name == "beat");
}

void serviceEntry() {
    using Kvasir::Test::test;
    test(
      "Service<>: the hook drives the bound supervisor after the entries before it, once a turn");
    static_assert(
      std::is_same_v<
        Kvasir::Startup::Detail::AllExtends<H::Service<>>,
        brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &H::Service<>::turn>>>);
    // no Hook::HealthCheck Extend of its own: listing it adds no check
    static_assert(brigand::size<Kvasir::Startup::Detail::AllExtends<H::Service<>>>::value == 1);
    int const fed = Watchdog<Producer>::feeds;
    CHECK(loopTurn() == Kvasir::Turn::within(10ms));   // the earliest: the supervisor's first tick
    CHECK(Watchdog<Producer>::feeds == fed + 1);       // started (no arm(): fed)
    for(int i = 0; i < 20; ++i) {
        advance(10ms);
        CHECK(loopTurn() == Kvasir::Turn::within(10ms));
    }
    CHECK(Producer::turns == 21 && Watchdog<Producer>::feeds == fed + 21);
    CHECK(Seen::all.empty());   // beats every turn: never starved
}
}   // namespace

void bootHalf() {
    using Kvasir::Test::test;
    test(
      "Service<>::runtimeInit: holds a stoppable watchdog off, reports the run before's starvation "
      "once");
    static_assert(Kvasir::Startup::ListRules::isHealthService<H::Service<>>);
    static_assert(HoldSup::canHoldOff && !Listed::canHoldOff);
    H::lastStarvation.store(H::Starvation{.check      = 0,
                                          .nameHash   = 0,
                                          .silentMs   = 60,
                                          .allowedMs  = 50,
                                          .uptimeMs   = 900,
                                          .silentMask = 1});
    H::Service<HoldTag>::runtimeInit();
    CHECK(StoppableWatchdog<HoldTag>::disarms == 1);
    CHECK(!H::lastStarvation.valid());   // logReport() took it
    test(
      "Service<>::runtimeInit: a watchdog that cannot be stopped is left alone, the report still "
      "runs");
    H::lastStarvation.store(H::Starvation{.check      = 0,
                                          .nameHash   = 0,
                                          .silentMs   = 60,
                                          .allowedMs  = 50,
                                          .uptimeMs   = 900,
                                          .silentMask = 1});
    H::Service<>::runtimeInit();
    CHECK(!H::lastStarvation.valid());
    test("Service<>::runtimeInit after the first turn: holdOff() does nothing");
    (void)HoldSup::turn();
    H::Service<HoldTag>::runtimeInit();
    CHECK(StoppableWatchdog<HoldTag>::disarms == 1);
}

int main() {
    H::lastStarvation.clear();
    bootHalf();
    H::lastStarvation.clear();
    serviceEntry();
    turnIsService();
    return Kvasir::Test::report();
}
