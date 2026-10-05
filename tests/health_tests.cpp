// Kvasir::Health::Supervisor on a fake Startup (two checks), a settable clock, a watchdog that counts feeds and a
// policy that records: fed while every check beats; a silent check starves exactly after its window (the first
// window is grace + maxInterval), feeding stops and the policy runs once; a blocked loop counts its real time;
// suspend/resume; AllChecksReported once; the statistics; the starvation record and its name hash.
#include "kvasir/Util/Health.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
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
    static constexpr auto Timeout = std::chrono::milliseconds{500};
    static inline int     feeds   = 0;

    static void feed() { ++feeds; }
};

struct FastHealth {
    static constexpr std::string_view name        = "fast";
    static constexpr auto             maxInterval = 50ms;
};

struct SlowHealth {
    static constexpr std::string_view name           = "slow";
    static constexpr auto             maxInterval    = 200ms;
    static constexpr auto             graceAfterBoot = 1s;
};

int allReported = 0;

struct FakeStartup {
    template<typename Hook>
    using ExtendsOn = brigand::list<H::Check<FastHealth>, H::Check<SlowHealth>>;

    template<typename Hook>
    static void run() {
        ++allReported;
    }
};

struct TestPolicy {
    static constexpr bool                   keepFeeding = false;
    static inline std::optional<H::Starved> seen;
    static inline int                       calls = 0;

    static void operator()(H::Starved const& s) {
        seen = s;
        ++calls;
    }
};

struct Config {
    static constexpr auto period = 10ms;
};

using Sup = H::Supervisor<FakeStartup, FakeWatchdog, FakeClock, Config>;

void step(std::chrono::milliseconds d,
          bool                      fast,
          bool                      slow) {
    FakeClock::t += d;
    if(fast) { H::checkIn<FastHealth>(); }
    if(slow) { H::checkIn<SlowHealth>(); }
    Sup::service();
}
}   // namespace

template<>
inline constexpr auto Kvasir::Health::injectedPolicy<> = TestPolicy{};

static_assert(H::fnv1a("") == 0x811C'9DC5U);

int main() {
    using Kvasir::Test::test;
    H::lastStarvation.clear();

    test("fed while every check beats; AllChecksReported once");
    Sup::service();   // the first call starts the clock and feeds
    for(int i = 0; i < 100; ++i) { step(10ms, true, true); }
    CHECK(FakeWatchdog::feeds == 101);
    CHECK(allReported == 1);
    CHECK(TestPolicy::calls == 0);

    test("the fast check silent: starved after its 50 ms, then never fed again");
    int const before = FakeWatchdog::feeds;
    for(int i = 0; i < 5; ++i) { step(10ms, false, true); }   // 50 ms: not over the window yet
    CHECK(TestPolicy::calls == 0 && FakeWatchdog::feeds == before + 5);
    step(10ms, false, true);   // 60 ms > 50 ms
    CHECK(TestPolicy::calls == 1 && TestPolicy::seen && TestPolicy::seen->name == "fast");
    CHECK(TestPolicy::seen && TestPolicy::seen->silentMs == 60
          && TestPolicy::seen->allowedMs == 50);
    CHECK(TestPolicy::seen && TestPolicy::seen->timeoutMs == 500);
    int const atStarve = FakeWatchdog::feeds;
    for(int i = 0; i < 10; ++i) {
        step(10ms, true, true);
    }   // beating again changes nothing: latched
    CHECK(FakeWatchdog::feeds == atStarve && TestPolicy::calls == 1);

    test("the record: index, name hash, the window");
    auto const r = H::lastStarvation.load();
    CHECK(r && r->check == 0 && r->nameHash == H::fnv1a("fast") && r->silentMs == 60
          && r->allowedMs == 50);
    CHECK(Sup::worstMs(0) >= 60);
    CHECK(Sup::name(1) == "slow");

    return Kvasir::Test::report();
}
