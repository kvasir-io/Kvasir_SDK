// Kvasir::Profile::HookProfiler (Runtime/LoopProfiler.hpp) injected into Startup::run / runTurn (Port/Extends.hpp)
// with a fake time source the entries advance: per-entry count / total / worst / last with the calibrated overhead
// subtracted, the loop's turns, period, worst and slips over the budget, busy turns from runTurn, the self-linked
// list, clearing; entry names "Class::fn" on gcc and clang; a hook not listed in Config::hooks is not profiled; the
// specialisation is declared AFTER the first use in this file and still wins.
#include "kvasir/Port/Extends.hpp"
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
        static void voidTurn() { FakeTime::t += 100; }

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
using List = brigand::list<Ext<Main, &Drivers::Bus<0>::voidTurn>,
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
    Kvasir::Startup::Detail::dispatch<Main, void>(static_cast<brigand::list<P1>*>(nullptr));
}

Kvasir::Turn turnOnce() {
    return Kvasir::Startup::Detail::dispatch<Main, Kvasir::Turn>(
      static_cast<brigand::list<P2>*>(nullptr));
}

void runOther() {
    Kvasir::Startup::Detail::dispatch<Other, void>(static_cast<brigand::list<P1>*>(nullptr));
}
}   // namespace

// after the functions that use it: the lookup is at instantiation
#include "kvasir/Runtime/LoopProfiler.hpp"

namespace {
struct LoopProfile {
    using TimeSource                 = FakeTime;
    static constexpr auto turnBudget = 150us;
};

using Prof = Kvasir::Profile::HookProfiler<LoopProfile>;

// LoopReport over its own profiler (a Config of its own), so the checks above see none of its clears
struct ReportProfile {
    using TimeSource = FakeTime;
};

constexpr auto reportPeriod = 1s;
using Report                = Kvasir::Profile::LoopReport<ReportProfile, reportPeriod>;
using ReportProf            = Kvasir::Profile::HookProfiler<ReportProfile>;
static_assert(Report::periodTicks == 1'000'000);
}   // namespace

template<>
struct Kvasir::Startup::HookProfiler<Kvasir::Tag::User> {
    using type = Prof;
};

int main() {
    using Kvasir::Test::test;
    using BusStats          = decltype(Prof::stats<Main, Ext<Main, &Drivers::Bus<0>::voidTurn>>);
    auto const& bus         = Prof::stats<Main, Ext<Main, &Drivers::Bus<0>::voidTurn>>;
    auto const& sensor      = Prof::stats<Main, Ext<Main, &Drivers::Sensor::poll>>;
    auto const& busTurn     = Prof::stats<Main, Ext<Main, &Drivers::Bus<1>::turn>>;
    auto const& otherSensor = Prof::stats<Other, Ext<Other, &Drivers::Sensor::other>>;
    static_assert(std::is_same_v<BusStats, Kvasir::Profile::EntryStats>);

    test("names: Class::fn, template arguments dropped");
    CHECK(Kvasir::Profile::entryName<&Drivers::Bus<0>::voidTurn> == "Bus::voidTurn");
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
    for(auto const* s = Prof::first(); s != nullptr; s = s->next) { ++n; }
    CHECK(n == 4);   // Bus<0>, Sensor::poll, Bus<1>::turn, Sensor::idle
    Prof::clear<Main>();
    CHECK(bus.count == 0 && bus.name == "Bus::voidTurn" && Prof::loop<Main>.turns == 0);
    runOnce();
    n = 0;
    for(auto const* s = Prof::first(); s != nullptr; s = s->next) { ++n; }
    CHECK(n == 4 && bus.count == 1);

    test(
      "LoopReport: arms, waits out the period, reports after exactly one period, across a 32-bit "
      "wrap");
    FakeTime::perRead = 0;
    FakeTime::t       = 0xFFFF'0000U;   // the period below crosses the wrap
    CHECK(Report::turn().in() == std::chrono::microseconds{1'000'000});   // first call: arms
    auto& rl = ReportProf::loop<Main>;
    rl.turns = 5;   // stands for a period's statistics
    FakeTime::t += 999'999;
    CHECK(Report::turn().in() == std::chrono::microseconds{1});   // one tick early: wait the rest
    CHECK(rl.turns == 5);
    FakeTime::t += 1;
    CHECK(Report::turn().in()
          == std::chrono::microseconds{1'000'000});   // reported (and cleared), re-armed
    CHECK(rl.turns == 0);
    rl.turns = 3;
    FakeTime::t += 500'000;
    CHECK(Report::turn().in() == std::chrono::microseconds{500'000} && rl.turns == 3);
    FakeTime::t += 500'000;
    CHECK(Report::turn().in() == std::chrono::microseconds{1'000'000} && rl.turns == 0);

    return Kvasir::Test::report();
}
