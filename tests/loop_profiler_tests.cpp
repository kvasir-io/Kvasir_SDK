// Kvasir::Profile::HookProfiler (Util/LoopProfiler.hpp) injected into Startup::run / runTurn (StartUp/Hooks.hpp)
// with a fake time source the entries advance: per-entry count / total / worst / last with the calibrated overhead
// subtracted, the loop's turns, period, worst and slips over the budget, busy turns from runTurn, the self-linked
// list, clearing; entry names "Class::fn" on gcc and clang; a hook not listed in Config::hooks is not profiled; the
// specialisation is declared AFTER the first use in this file and still wins.
#include "kvasir/StartUp/Hooks.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

using namespace std::chrono_literals;

namespace {
struct FakeTime {
    static constexpr std::uint64_t    ticksPerSecond = 1'000'000;
    static constexpr std::string_view name           = "fake";
    static inline std::uint32_t       t              = 0;
    static inline std::uint32_t       perRead        = 0;   // what a read costs

    static std::uint32_t now() {
        t += perRead;
        return t;
    }
};

struct Other {
    using Signature = void();
};

namespace Drivers {
    template<int N>
    struct Bus {
        static void handler() { FakeTime::t += 100; }

        static Kvasir::Turn turn() {
            FakeTime::t += 7;
            return Kvasir::Turn::busy();
        }
    };

    struct Sensor {
        static void poll() { FakeTime::t += 20; }

        static bool idle() { return false; }

        static void other() { FakeTime::t += 1'000; }
    };
}   // namespace Drivers

using Main = Kvasir::Hook::MainLoop;
template<typename H, auto F>
using Ext  = Kvasir::Startup::Extend<H, F>;
using List = brigand::list<Ext<Main, &Drivers::Bus<0>::handler>,
                           Ext<Main, &Drivers::Sensor::poll>,
                           Ext<Other, &Drivers::Sensor::other>>;

struct P1 {
    using Extends = List;
};

struct P2 {
    using Extends
      = brigand::list<Ext<Main, &Drivers::Bus<1>::turn>, Ext<Main, &Drivers::Sensor::idle>>;
};

void runOnce() {
    Kvasir::Startup::Detail::runHookOf<Main>(static_cast<brigand::list<P1>*>(nullptr));
}

Kvasir::Turn turnOnce() {
    return Kvasir::Startup::Detail::runTurnOf<Main>(static_cast<brigand::list<P2>*>(nullptr));
}

void runOther() {
    Kvasir::Startup::Detail::runHookOf<Other>(static_cast<brigand::list<P1>*>(nullptr));
}
}   // namespace

// after the functions that use it: the lookup is at instantiation
#include "kvasir/Util/LoopProfiler.hpp"

namespace {
struct LoopProfile {
    using TimeSource                 = FakeTime;
    static constexpr auto turnBudget = 150us;
};

using Prof = Kvasir::Profile::HookProfiler<LoopProfile>;
}   // namespace

template<>
inline constexpr auto Kvasir::Startup::injectedHookProfiler<> = Prof{};

int main() {
    using Kvasir::Test::test;
    using BusStats    = decltype(Prof::stats<Main, Ext<Main, &Drivers::Bus<0>::handler>>);
    auto& bus         = Prof::stats<Main, Ext<Main, &Drivers::Bus<0>::handler>>;
    auto& sensor      = Prof::stats<Main, Ext<Main, &Drivers::Sensor::poll>>;
    auto& busTurn     = Prof::stats<Main, Ext<Main, &Drivers::Bus<1>::turn>>;
    auto& otherSensor = Prof::stats<Other, Ext<Other, &Drivers::Sensor::other>>;
    static_assert(std::is_same_v<BusStats, Kvasir::Profile::EntryStats>);

    test("names: Class::fn, template arguments dropped");
    CHECK(Kvasir::Profile::entryName<&Drivers::Bus<0>::handler> == "Bus::handler");
    CHECK(Kvasir::Profile::entryName<&Drivers::Sensor::poll> == "Sensor::poll");

    test("calibration: the cost of one measurement is subtracted");
    CHECK(FakeTime::name == "fake");
    FakeTime::perRead = 2;
    Prof::calibrate();
    CHECK(Prof::overhead() == 2);

    test("entries: count, total, worst, last; the loop's turns, period and slips");
    for(int i = 0; i != 3; ++i) {
        runOnce();
        FakeTime::t += (i == 1 ? 200 : 10);   // between turns: one long gap
    }
    CHECK(bus.count == 3 && bus.totalTicks == 300 && bus.worstTicks == 100 && bus.lastTicks == 100);
    CHECK(sensor.count == 3 && sensor.totalTicks == 60);
    auto const& l = Prof::loop<Main>;
    CHECK(l.turns == 3
          && l.slips
               == 1);   // the turn after the 200-tick gap starts > 150 us after the one before
    CHECK(l.worstPeriodTicks > 300 && l.periodTicks > 0 && l.hookTicks >= 360);

    test("a hook not in Config::hooks is not profiled");
    runOther();
    CHECK(otherSensor.count == 0);

    test("runTurn: profiled, busy turns counted, the earliest Turn still returned");
    auto const t = turnOnce();
    CHECK(t.isBusy() && busTurn.count == 1 && busTurn.busyTurns == 1 && busTurn.totalTicks == 7);

    test("the list links every entry once; clear keeps the list");
    int n = 0;
    for(auto* s = Prof::first(); s != nullptr; s = s->next) { ++n; }
    CHECK(n == 4);   // Bus<0>, Sensor::poll, Bus<1>::turn, Sensor::idle
    Prof::clear<Main>();
    CHECK(bus.count == 0 && bus.name == "Bus::handler" && Prof::loop<Main>.turns == 0);
    runOnce();
    n = 0;
    for(auto* s = Prof::first(); s != nullptr; s = s->next) { ++n; }
    CHECK(n == 4 && bus.count == 1);

    return Kvasir::Test::report();
}
