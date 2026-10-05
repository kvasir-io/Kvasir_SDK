// Kvasir::Executor (Util/Executor.hpp) and Startup::runTurn (StartUp/Hooks.hpp) on the host, with a fake clock, a
// fake wake source and a "sleep" that moves the clock to the armed time: due order, equal times in arming order,
// once/cancel, cancel and re-arm from the own callback; catchUp after a stall runs every missed period (MaxPerTurn
// per turn), skip once with missed(), rebase from now - each as Every::due/dueCount/skipTo; turn() says when it needs
// the loop; a Deferred posted three times runs once, posted from its own callback it runs next turn, posts from two
// threads all arrive; adopt() links Hook::Every entries with their phase and ignores the rest; sleep() arms the
// earliest of hint, head and MaxSleep, sleeps through to it and not at all when busy or kicked; lateness and the
// statistics; runTurn folds void/bool/Turn, earliest wins, and run<Hook>() still takes Turn-returning functions.
#include "kvasir/Util/Executor.hpp"

#include "kvasir/StartUp/Hooks.hpp"
#include "support/FakeClock.hpp"
#include "support/FakeWake.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using Clock = Kvasir::Test::FakeClockT<std::chrono::microseconds>;
using Wake  = Kvasir::Test::FakeWake<Clock>;
using Sleep = Kvasir::Test::HostSleep<Clock>;

namespace {
struct PolledConfig {
    using Clock = ::Clock;
};

struct SleepingConfig {
    using Clock                      = ::Clock;
    using Wake                       = ::Wake;
    using Sleep                      = ::Sleep;
    static constexpr auto maxSleep   = 1s;
    static constexpr auto maxPerTurn = 4U;
};

using Exec  = Kvasir::Executor<PolledConfig>;
using SExec = Kvasir::Executor<SleepingConfig>;

std::vector<int> order;

constexpr auto TickPeriod = 250ms;
constexpr auto TickPhase  = 3ms;
int            ticks      = 0;

struct Ticker {
    static void tick() { ++ticks; }

    using Extends
      = Kvasir::Startup::Extend<Kvasir::Hook::Every<TickPeriod, TickPhase>, &Ticker::tick>;
};

struct Busy {
    static void handler() {}

    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Busy::handler>;
};

struct FakeStartup {
    using LocalPeripherals = brigand::list<Ticker, Busy>;
};

// hook functions for runTurn
int voidRuns = 0;

void voidFn() { ++voidRuns; }

bool falseFn() { return false; }

bool trueFn() { return true; }

Kvasir::Turn in5() { return Kvasir::Turn::within(5ms); }

Kvasir::Turn in2() { return Kvasir::Turn::within(2ms); }

using Main = Kvasir::Hook::MainLoop;
template<auto F>
using On = Kvasir::Startup::Extend<Main, F>;

template<typename... Es>
Kvasir::Turn fold() {
    return Kvasir::Startup::Detail::runTurn<Main>(static_cast<brigand::list<Es...>*>(nullptr));
}

// the callbacks' state (a callback is a plain function)
int ups = 0, skipsRun = 0, rebases = 0, runs = 0, seen = 0;

Exec::Timer    self{};
Exec::Timer    again{};
Exec::Deferred again2{[] {
    ++runs;
    if(runs == 1) { Exec::post(again2); }
}};

void reboot() {
    Exec::reset();
    SExec::reset();
    Clock::reset();
    Wake::disarm();
    order.clear();
}
}   // namespace

int main() {
    using Kvasir::Turn;
    using Kvasir::Test::test;

    test(
      "runTurn: void is busy, bool false idle, true busy, Turn as it says; the earliest wins; all "
      "run");
    CHECK(fold<On<&falseFn>>().isIdle());
    CHECK(fold<On<&trueFn>>().isBusy());
    CHECK(fold<On<&voidFn>>().isBusy() && voidRuns == 1);
    CHECK((fold<On<&in5>, On<&falseFn>, On<&in2>>() == Turn::within(2ms)));
    CHECK((fold<On<&in5>, On<&voidFn>>().isBusy()) && voidRuns == 2);
    CHECK(fold<>().isIdle());
    CHECK(Turn::within(0ms).isBusy() && Turn::within(-3ms).isBusy()
          && Turn::within(1ns) == Turn::within(1us));
    Kvasir::Startup::Detail::runHook<Main>(
      static_cast<brigand::list<On<&in5>, On<&voidFn>>*>(nullptr));
    CHECK(voidRuns == 3);   // run<Hook>() takes a Turn-returning function and drops the value

    test("order: due order, equal times in arming order; once unlinks; cancel");
    reboot();
    Exec::Timer a{[] { order.push_back(1); }}, b{[] { order.push_back(2); }},
      c{[] { order.push_back(3); }}, d{[] { order.push_back(4); }};
    Exec::in(a, 30ms);
    Exec::in(b, 10ms);
    Exec::in(c, 10ms);
    Exec::in(d, 20ms);
    CHECK(Exec::cancel(d) && !Exec::cancel(d) && !d.armed());
    CHECK(Exec::turn() == Turn::within(10ms));
    Clock::advance(30ms);
    CHECK(Exec::turn().isIdle());
    CHECK((order == std::vector<int>{2, 3, 1}) && !a.armed() && !b.armed());

    test("a callback cancels itself, another re-arms itself");
    reboot();
    self.setCallback([] {
        order.push_back(7);
        Exec::cancel(self);
    });
    Exec::every(self, 5ms);
    again.setCallback([] {
        order.push_back(8);
        if(order.size() < 4) { Exec::in(again, 1ms); }
    });
    Exec::in(again, 1ms);
    for(int i = 0; i != 20; ++i) {
        Clock::advance(1ms);
        (void)Exec::turn();
    }
    CHECK((order == std::vector<int>{7, 8, 8, 8}) && !self.armed() && !again.armed());

    test(
      "catchUp after a 4-period stall: every missed period runs, MaxPerTurn a turn, on the old "
      "grid");
    reboot();
    SExec::Timer up{[] { ++ups; }}, skip{[] { ++skipsRun; }}, re{[] { ++rebases; }};
    SExec::every(up, 10ms, Kvasir::Repeat::catchUp);
    SExec::every(skip, 10ms, Kvasir::Repeat::skip);
    SExec::every(re, 10ms, Kvasir::Repeat::rebase);
    (void)SExec::turn();   // the first runs at once
    CHECK(ups == 1 && skipsRun == 1 && rebases == 1);
    Clock::advance(43ms);            // due at 10, 20, 30, 40
    CHECK(SExec::turn().isBusy());   // 4 callbacks: MaxPerTurn
    while(SExec::turn().isBusy()) {}
    CHECK(ups == 5 && skipsRun == 2 && skip.missed() == 3 && rebases == 2);
    CHECK(up.due() == Clock::time_point{50ms} && skip.due() == Clock::time_point{50ms}
          && re.due() == Clock::time_point{53ms});

    test("Deferred: three posts run once; posted from its own callback it runs next turn");
    reboot();
    Exec::post(again2);
    Exec::post(again2);
    Exec::post(again2);
    (void)Exec::turn();
    CHECK(runs == 1);
    (void)Exec::turn();
    CHECK(runs == 2);
    (void)Exec::turn();
    CHECK(runs == 2);

    test("Deferred: posts from two threads all arrive");
    reboot();
    Exec::Deferred dA{[] { ++seen; }}, dB{[] { ++seen; }};
    int            total = 0;
    std::thread    t1{[&] {
        for(int i = 0; i != 2000; ++i) { Exec::post(dA); }
    }};
    std::thread    t2{[&] {
        for(int i = 0; i != 2000; ++i) { Exec::post(dB); }
    }};
    t1.join();
    t2.join();
    (void)Exec::turn();
    total = seen;
    CHECK(total == 2);

    test("adopt: Hook::Every entries, their phase first; other hooks ignored");
    reboot();
    Exec::adopt<FakeStartup>();
    CHECK(Exec::turn() == Turn::within(3ms) && ticks == 0);
    Clock::advance(3ms);
    (void)Exec::turn();
    CHECK(ticks == 1);
    Clock::advance(250ms);
    (void)Exec::turn();
    CHECK(ticks == 2 && Exec::turn() == Turn::within(250ms));

    test("sleep: arms the earliest of hint, head and MaxSleep; busy or kicked does not sleep");
    reboot();
    SExec::Timer late{[] {}};
    SExec::in(late, 20ms);
    SExec::sleep(Turn::busy());
    CHECK(Clock::now() == Clock::time_point{} && !Wake::armedAt);
    SExec::kick();
    SExec::sleep(Turn::within(5ms));
    CHECK(Clock::now() == Clock::time_point{});
    SExec::sleep(Turn::within(5ms));
    CHECK(Clock::now() == Clock::time_point{5ms});
    SExec::sleep(Turn::idle());
    CHECK(Clock::now() == Clock::time_point{20ms});
    (void)SExec::turn();
    SExec::sleep(Turn::idle());   // nothing armed: MaxSleep
    CHECK(Clock::now() == Clock::time_point{20ms + 1s});
    SExec::Deferred dd{[] {}};
    SExec::post(dd);
    auto const before = Clock::now();
    SExec::sleep(Turn::idle());
    CHECK(Clock::now() == before);   // posted work: no sleep

    test("lateness and the statistics");
    reboot();
    SExec::Timer five{[] {}};
    SExec::in(five, 1ms);
    Clock::advance(6ms);
    (void)SExec::turn();
    auto const st = SExec::takeStats();
    CHECK(st.worstLate == 5ms && st.callbacks == 1 && st.turns == 1);
    CHECK(SExec::takeStats().callbacks == 0);

    static_assert(Kvasir::Detail::typeNameOf<Ticker>().find("Ticker") != std::string_view::npos);
    return Kvasir::Test::report();
}
