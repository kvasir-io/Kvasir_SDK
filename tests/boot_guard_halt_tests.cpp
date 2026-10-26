// Kvasir::BootGuard::OnLimit::Halt<Startup, Stop> (Runtime/BootGuardRules.hpp): what stops the watchdog. Halt's operator()
// never returns, so it is only instantiated here (its address taken); its watchdog part, stopWatchdog(), runs: with
// no watchdog (void), with an ungated watchdog's plain disarm(), and with a gated watchdog through the
// Health::Supervisor that owns its key (holdOff(): the keyed disarm, before the first service() only). The refusals
// (a gated watchdog passed directly, a supervisor whose watchdog cannot be stopped) are boot_guard_must_fail's.
#include "kvasir/Health/Health.hpp"
#include "kvasir/Runtime/BootGuardRules.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace BG = Kvasir::BootGuard;
namespace H  = Kvasir::Health;
using namespace std::chrono_literals;

namespace {
using FakeClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

struct Loop {
    static constexpr std::string_view name        = "loop";
    static constexpr auto             maxInterval = 50ms;
};

// a Startup for both: the supervisor's check list and the SafeState hook Halt runs
struct Startup {
    static inline int safeStates = 0;

    static void safeState() { ++safeStates; }

    template<typename Hook>
    using ExtendsOn = std::conditional_t<
      std::is_same_v<Hook, Kvasir::Hook::SafeState>,
      brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::SafeState, &safeState>>,
      brigand::list<H::Check<Loop>>>;

    template<typename Hook>
    static void run() {}
};

// an ungated watchdog: disarm() takes no key
struct PlainWatchdog {
    static inline int disarms = 0;

    static void disarm() { ++disarms; }
};

// a gated watchdog, as chip_rp_common's with Config::gatedFeed: every call takes the supervisor's key
struct GatedWatchdog {
    static constexpr auto timeout = 500ms;
    static inline int     disarms = 0;
    static inline int     feeds   = 0;

    static void feed(H::FeedKey) { ++feeds; }

    static void disarm(H::FeedKey) { ++disarms; }
};

struct Period10 {
    static constexpr auto period = 10ms;
};

using Supervisor = H::Supervisor<Startup, GatedWatchdog, FakeClock, Period10>;

// operator() is compiled for each form (its address taken), never called: it ends in a wfi loop
[[maybe_unused]] constexpr auto haltVoid  = &BG::OnLimit::Halt<Startup>::operator();
[[maybe_unused]] constexpr auto haltPlain = &BG::OnLimit::Halt<Startup, PlainWatchdog>::operator();
[[maybe_unused]] constexpr auto haltSupervised
  = &BG::OnLimit::Halt<Startup, Supervisor>::operator();

static_assert(Supervisor::canHoldOff,
              "a gated watchdog with disarm(FeedKey) can be held off");
}   // namespace

int main() {
    using Kvasir::Test::test;

    test("Halt<Startup>: no watchdog, nothing to stop");
    BG::OnLimit::Halt<Startup>::stopWatchdog();
    CHECK_EQ(PlainWatchdog::disarms, 0);
    CHECK_EQ(GatedWatchdog::disarms, 0);

    test("Halt<Startup, W>: an ungated watchdog's own disarm()");
    BG::OnLimit::Halt<Startup, PlainWatchdog>::stopWatchdog();
    CHECK_EQ(PlainWatchdog::disarms, 1);

    test(
      "Halt<Startup, Supervisor>: the gated watchdog through holdOff(), before the first "
      "service()");
    BG::OnLimit::Halt<Startup, Supervisor>::stopWatchdog();
    CHECK_EQ(GatedWatchdog::disarms, 1);

    test(
      "after the supervisor's first service() holdOff() no longer stops it (the guard runs before "
      "that)");
    Supervisor::service();
    BG::OnLimit::Halt<Startup, Supervisor>::stopWatchdog();
    CHECK_EQ(GatedWatchdog::disarms, 1);
    CHECK(GatedWatchdog::feeds >= 1);
    CHECK_EQ(Startup::safeStates, 0);
    return Kvasir::Test::report();
}
