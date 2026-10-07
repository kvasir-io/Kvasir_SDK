// Kvasir::Health::Supervisor under a policy that keeps feeding (LogOnly's shape): the watchdog is fed through a
// starvation, the check gets a fresh window and is reported again each time it runs out, the record holds the
// latest, and a check that beats again is simply healthy.
#include "kvasir/Util/Health.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace H = Kvasir::Health;
using namespace std::chrono_literals;

namespace {
struct FakeClock {
    using rep        = std::int64_t;
    using period     = std::milli;
    using duration   = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<FakeClock>;
    static inline time_point t{};

    static time_point now() { return t; }
};

struct FakeWatchdog {
    static inline int feeds = 0;   // no Timeout member: the supervisor reports 0 for it

    static void feed() { ++feeds; }
};

struct OnlyHealth {
    static constexpr std::string_view name        = "only";
    static constexpr auto             maxInterval = 50ms;
};

struct FakeStartup {
    template<typename Hook>
    using ExtendsOn = brigand::list<H::Check<OnlyHealth>>;

    template<typename Hook>
    static void run() {}
};

struct KeepFeeding {
    static constexpr bool    keepFeeding = true;
    static inline int        calls       = 0;
    static inline H::Starved last{};

    static void operator()(H::Starved const& s) {
        last = s;
        ++calls;
    }
};

using Sup = H::Supervisor<FakeStartup, FakeWatchdog, FakeClock>;   // the default Config: 10 ms

void turn(bool beat) {
    FakeClock::t += 10ms;
    if(beat) { H::checkIn<OnlyHealth>(); }
    Sup::service();
}
}   // namespace

template<>
inline constexpr auto Kvasir::Health::injectedPolicy<> = KeepFeeding{};

static_assert(H::LogOnly::keepFeeding && !H::StopFeeding::keepFeeding
              && !H::RaisePanic::keepFeeding);
static_assert(Sup::periodMs == 10 && Sup::timeoutMs == 0);

int main() {
    using Kvasir::Test::test;
    H::lastStarvation.clear();

    test("keepFeeding: fed through the starvation, reported once per window");
    Sup::service();
    turn(true);
    int const fed = FakeWatchdog::feeds;
    for(int i = 0; i < 6; ++i) { turn(false); }   // 60 ms > 50 ms
    CHECK(KeepFeeding::calls == 1 && KeepFeeding::last.silentMs == 60
          && KeepFeeding::last.timeoutMs == 0);
    CHECK(FakeWatchdog::feeds == fed + 6);
    for(int i = 0; i < 5; ++i) { turn(false); }   // a fresh window: 50 ms more is not over
    CHECK(KeepFeeding::calls == 1);
    turn(false);
    CHECK(KeepFeeding::calls == 2 && FakeWatchdog::feeds == fed + 12);

    test("the record holds the latest starvation");
    auto const r = H::lastStarvation.load();
    CHECK(r && r->check == 0 && r->silentMs == 60 && r->nameHash == H::fnv1a("only"));

    test("beating again: healthy, no further report");
    for(int i = 0; i < 100; ++i) { turn(true); }
    CHECK(KeepFeeding::calls == 2 && FakeWatchdog::feeds == fed + 112);
    CHECK(Sup::worstMs(0) == 60);

    return Kvasir::Test::report();
}
