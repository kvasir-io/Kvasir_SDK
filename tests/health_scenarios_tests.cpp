// Kvasir::Health::Supervisor, the cases health_tests.cpp leaves out. A supervisor's state is static, so each
// scenario has its own instantiation (a tag on the checks, the Startup and the watchdog): the first window (grace +
// maxInterval, then maxInterval), a blocked loop counted at its real length, suspend / resume / startSuspended,
// the gated feed (only the supervisor's FeedKey feeds), two checks starving in one turn, the silent mask, the
// record taken once - and two properties over random loop timing: a check that beats every turn is never starved,
// and one that falls silent is named within one period plus one loop turn of its window.
#include "kvasir/Util/Health.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <string_view>
#include <type_traits>
#include <vector>

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

// what the policy saw, for every scenario
struct Seen {
    static inline std::vector<H::Starved> all;
};

struct TestPolicy {
    static constexpr bool keepFeeding = false;

    static void operator()(H::Starved const& s) { Seen::all.push_back(s); }
};

template<int Tag>
struct Watchdog {
    static constexpr auto Timeout = std::chrono::milliseconds{500};
    // not every scenario reads it
    [[maybe_unused]] static inline int feeds = 0;

    static void feed() { ++feeds; }
};

// a gated watchdog, as chip_rp_common's with Config::gatedFeed: no plain feed()
template<int Tag>
struct GatedWatchdog {
    static constexpr auto Timeout = std::chrono::milliseconds{500};
    // not every scenario reads it
    [[maybe_unused]] static inline int feeds = 0;

    static void feed(H::FeedKey) { ++feeds; }
};

template<int Tag>
[[maybe_unused]] int allReported = 0;

template<int Tag, typename... Cs>
struct Startup {
    template<typename Hook>
    using ExtendsOn = brigand::list<H::Check<Cs>...>;

    template<typename Hook>
    static void run() {
        ++allReported<Tag>;
    }
};

struct Period10 {
    static constexpr auto period = 10ms;
};

template<int Tag>
struct A {   // 50 ms, the default grace (= maxInterval)
    static constexpr std::string_view name        = "a";
    static constexpr auto             maxInterval = 50ms;
};

template<int Tag>
struct B {   // 100 ms
    static constexpr std::string_view name        = "b";
    static constexpr auto             maxInterval = 100ms;
};

template<int Tag>
struct Graced {   // 50 ms after a boot grace of 200 ms
    static constexpr std::string_view name           = "graced";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             graceAfterBoot = 200ms;
};

template<int Tag>
struct Parked {   // starts suspended
    static constexpr std::string_view name           = "parked";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr bool             startSuspended = true;
};

template<int Tag, typename... Cs>
using Sup = H::Supervisor<Startup<Tag, Cs...>, Watchdog<Tag>, FakeClock, Period10>;

void advance(std::chrono::milliseconds d) { FakeClock::t += d; }

// the policy calls a scenario caused
struct Calls {
    std::size_t before = Seen::all.size();

    [[nodiscard]] std::size_t count() const { return Seen::all.size() - before; }

    [[nodiscard]] H::Starved const& last() const { return Seen::all.back(); }
};

// ---- the first window ------------------------------------------------------------------------
void firstWindow() {
    using Kvasir::Test::test;
    test(
      "a check that never beats: starved after grace + maxInterval (default grace = maxInterval)");
    {
        using S = Sup<1, A<1>>;
        Calls const calls;
        S::service();
        for(int i = 0; i < 10; ++i) {   // 100 ms = 50 + 50: not over yet
            advance(10ms);
            S::service();
        }
        CHECK(calls.count() == 0 && Watchdog<1>::feeds == 11);
        advance(10ms);
        S::service();
        CHECK(calls.count() == 1 && calls.last().silentMs == 110 && calls.last().allowedMs == 100);
        CHECK(Watchdog<1>::feeds == 11);
        CHECK(allReported<1> == 0);
    }
    test("graceAfterBoot: the first window is 200 + 50 ms, after the first beat 50 ms");
    {
        using S = Sup<2, Graced<2>>;
        Calls const calls;
        S::service();
        for(int i = 0; i < 25; ++i) {
            advance(10ms);
            S::service();
        }
        CHECK(calls.count() == 0);   // 250 ms silent: still inside the first window
        H::checkIn<Graced<2>>();
        advance(10ms);
        S::service();
        CHECK(allReported<2> == 1);
        for(int i = 0; i < 5; ++i) {
            advance(10ms);
            S::service();
        }
        CHECK(calls.count() == 0);   // 50 ms since the beat
        advance(10ms);
        S::service();
        CHECK(calls.count() == 1 && calls.last().silentMs == 60 && calls.last().allowedMs == 50);
    }
    test("graceAfterBoot: silent from boot is starved right after 250 ms");
    {
        using S = Sup<3, Graced<3>>;
        Calls const calls;
        S::service();
        for(int i = 0; i < 25; ++i) {
            advance(10ms);
            S::service();
        }
        CHECK(calls.count() == 0);
        advance(10ms);
        S::service();
        CHECK(calls.count() == 1 && calls.last().silentMs == 260 && calls.last().allowedMs == 250);
    }
}

// ---- a blocked loop --------------------------------------------------------------------------
void blockedLoop() {
    using Kvasir::Test::test;
    test(
      "a loop blocked 200 ms: a check that beat in the turn after it is not starved, and is fed");
    {
        using S = Sup<4, A<4>>;
        Calls const calls;
        S::service();
        H::checkIn<A<4>>();
        advance(10ms);
        S::service();
        int const fed = Watchdog<4>::feeds;
        advance(200ms);       // a flash erase, a log flood: no turn
        H::checkIn<A<4>>();   // the MainLoop hooks run before service()
        S::service();
        CHECK(calls.count() == 0 && Watchdog<4>::feeds == fed + 1);
        CHECK(S::worstMs(0) == 0);
    }
    test("a loop blocked 200 ms with the check silent: the whole 200 ms count, starved at once");
    {
        using S = Sup<5, A<5>>;
        Calls const calls;
        S::service();
        H::checkIn<A<5>>();
        advance(10ms);
        S::service();
        int const fed = Watchdog<5>::feeds;
        advance(200ms);
        S::service();
        CHECK(calls.count() == 1 && calls.last().silentMs == 200 && calls.last().allowedMs == 50);
        CHECK(Watchdog<5>::feeds == fed);
        CHECK(S::worstMs(0) == 200);
    }
    test("turns faster than the period judge nothing and feed nothing in between");
    {
        using S = Sup<6, A<6>>;
        S::service();
        int const fed = Watchdog<6>::feeds;
        for(int i = 0; i < 9; ++i) {
            advance(1ms);
            S::service();
        }
        CHECK(Watchdog<6>::feeds == fed);
        advance(1ms);
        S::service();
        CHECK(Watchdog<6>::feeds == fed + 1);
    }
}

// ---- suspend, resume, startSuspended ---------------------------------------------------------
void suspended() {
    using Kvasir::Test::test;
    test("a suspended check is not judged; resume gives it a whole window");
    {
        using S = Sup<7, A<7>, B<7>>;
        Calls const calls;
        S::service();
        H::checkIn<A<7>>();
        H::checkIn<B<7>>();
        advance(10ms);
        S::service();
        S::suspend<A<7>>();
        for(int i = 0; i < 100; ++i) {   // a second of silence from a
            advance(10ms);
            H::checkIn<B<7>>();
            S::service();
        }
        CHECK(calls.count() == 0 && Watchdog<7>::feeds == 102);
        S::resume<A<7>>();
        for(int i = 0; i < 5; ++i) {
            advance(10ms);
            H::checkIn<B<7>>();
            S::service();
        }
        CHECK(calls.count() == 0);   // 50 ms since the resume
        advance(10ms);
        H::checkIn<B<7>>();
        S::service();
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().silentMs == 60);
    }
    test(
      "startSuspended: not judged, not waited for by AllChecksReported; resumed, its first window");
    {
        using S = Sup<8, A<8>, Parked<8>>;
        Calls const calls;
        S::service();
        for(int i = 0; i < 50; ++i) {
            advance(10ms);
            H::checkIn<A<8>>();
            S::service();
        }
        CHECK(calls.count() == 0 && allReported<8> == 1);
        S::resume<Parked<8>>();
        for(int i = 0; i < 10; ++i) {   // grace 50 + 50
            advance(10ms);
            H::checkIn<A<8>>();
            S::service();
        }
        CHECK(calls.count() == 0);
        advance(10ms);
        H::checkIn<A<8>>();
        S::service();
        CHECK(calls.count() == 1 && calls.last().name == "parked" && calls.last().check == 1);
        CHECK(allReported<8> == 1);
    }
    test("a beat while suspended is no starvation and no stale silence after the resume");
    {
        using S = Sup<9, A<9>>;
        Calls const calls;
        S::service();
        H::checkIn<A<9>>();
        advance(10ms);
        S::service();
        for(int i = 0; i < 4; ++i) {   // 40 ms silent, then suspended
            advance(10ms);
            S::service();
        }
        S::suspend<A<9>>();
        advance(500ms);
        S::service();
        S::resume<A<9>>();
        for(int i = 0; i < 5; ++i) {   // the 40 ms from before the suspension do not count
            advance(10ms);
            S::service();
        }
        CHECK(calls.count() == 0);
    }
}

// ---- the gate --------------------------------------------------------------------------------
// nothing but the supervisor makes a key: not default-constructible, no aggregate, no conversion
static_assert(!std::is_default_constructible_v<H::FeedKey>);
static_assert(!std::is_aggregate_v<H::FeedKey>);
template<typename W>
concept PlainFeed = requires { W::feed(); };
template<typename W>
concept BracedFeed = requires { W::feed({}); };
static_assert(!PlainFeed<GatedWatchdog<0>> && !BracedFeed<GatedWatchdog<0>>);

void gate() {
    using Kvasir::Test::test;
    test(
      "a gated watchdog (feed takes a FeedKey) is fed by the supervisor, and only while healthy");
    using S = H::Supervisor<Startup<10, A<10>>, GatedWatchdog<10>, FakeClock, Period10>;
    Calls const calls;
    S::service();
    for(int i = 0; i < 20; ++i) {
        advance(10ms);
        H::checkIn<A<10>>();
        S::service();
    }
    CHECK(GatedWatchdog<10>::feeds == 21 && calls.count() == 0);
    for(int i = 0; i < 20; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(calls.count() == 1);
    CHECK(GatedWatchdog<10>::feeds == 21 + 5);   // the five turns inside the window, none after
}

// ---- the watchdog's life: holdOff, armed at the first turn ------------------------------------
// a gated watchdog as chip_rp_common's: feed, arm and disarm all take the key
template<int Tag>
struct OwnedWatchdog {
    static constexpr auto              Timeout = std::chrono::milliseconds{500};
    [[maybe_unused]] static inline int feeds   = 0;
    [[maybe_unused]] static inline int arms    = 0;
    [[maybe_unused]] static inline int disarms = 0;

    static void feed(H::FeedKey) { ++feeds; }

    static void arm(H::FeedKey) { ++arms; }

    static void disarm(H::FeedKey) { ++disarms; }
};

// an ungated one with the same life
template<int Tag>
struct PlainWatchdog {
    [[maybe_unused]] static inline int feeds   = 0;
    [[maybe_unused]] static inline int arms    = 0;
    [[maybe_unused]] static inline int disarms = 0;

    static void feed() { ++feeds; }

    static void arm() { ++arms; }

    static void disarm() { ++disarms; }
};

template<typename W>
concept PlainArm = requires { W::arm(); };
template<typename W>
concept PlainDisarm = requires { W::disarm(); };
static_assert(!PlainArm<OwnedWatchdog<0>> && !PlainDisarm<OwnedWatchdog<0>>
              && !PlainFeed<OwnedWatchdog<0>>);

template<typename W,
         int Tag>
void lifeOf() {
    using S = H::Supervisor<Startup<Tag, A<Tag>>, W, FakeClock, Period10>;
    S::holdOff();
    CHECK(W::disarms == 1 && W::arms == 0 && W::feeds == 0);
    advance(700ms);   // a boot longer than the watchdog's timeout: nothing runs meanwhile
    S::service();     // the first turn arms, which loads the whole timeout
    CHECK(W::arms == 1 && W::feeds == 0);
    for(int i = 0; i < 5; ++i) {
        advance(10ms);
        H::checkIn<A<Tag>>();
        S::service();
    }
    CHECK(W::arms == 1 && W::feeds == 5);
    S::holdOff();   // once the supervisor runs it is no off switch
    CHECK(W::disarms == 1);
    for(int i = 0; i < 20; ++i) {   // starved: never armed, fed or stopped again
        advance(10ms);
        S::service();
        S::holdOff();
    }
    CHECK(W::arms == 1 && W::disarms == 1 && W::feeds == 5 + 5);
}

// holdOff() is only there over a watchdog that can be stopped
template<typename S>
concept HoldsOff = requires { S::holdOff(); };
static_assert(HoldsOff<H::Supervisor<Startup<0,
                                             A<0>>,
                                     OwnedWatchdog<0>,
                                     FakeClock,
                                     Period10>>);
static_assert(HoldsOff<H::Supervisor<Startup<0,
                                             A<0>>,
                                     PlainWatchdog<0>,
                                     FakeClock,
                                     Period10>>);
static_assert(!HoldsOff<H::Supervisor<Startup<0,
                                              A<0>>,
                                      GatedWatchdog<0>,
                                      FakeClock,
                                      Period10>>);

void lifecycle() {
    using Kvasir::Test::test;
    test(
      "a gated watchdog: holdOff stops it before the first turn only, the first turn arms, then "
      "feeds");
    lifeOf<OwnedWatchdog<13>, 13>();
    test("an ungated watchdog with arm()/disarm(): the same life through the plain calls");
    lifeOf<PlainWatchdog<14>, 14>();
    test("a watchdog without arm(): the first turn feeds");
    {
        using S = Sup<15, A<15>>;
        S::service();
        CHECK(Watchdog<15>::feeds == 1);
    }
}

// ---- which check, the mask, the record -------------------------------------------------------
void naming() {
    using Kvasir::Test::test;
    test(
      "two checks over their windows in one turn: the first in the list is named, the policy runs "
      "once");
    {
        using S = Sup<11, A<11>, B<11>>;
        Calls const calls;
        H::lastStarvation.clear();
        S::service();
        H::checkIn<A<11>>();
        H::checkIn<B<11>>();
        advance(10ms);
        S::service();
        advance(300ms);   // both far over
        S::service();
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().check == 0);
        auto const r = H::lastStarvation.load();
        CHECK(r && r->check == 0 && r->nameHash == H::fnv1a("a") && r->silentMask == 0b11U);
        advance(300ms);
        S::service();
        CHECK(calls.count() == 1);   // latched: b is not reported on top
    }
    test("the silent mask: checks past HALF their window, a beating one not");
    {
        using S = Sup<12, B<12>, A<12>, Graced<12>>;   // b 100 ms, a 50 ms, graced beating
        Calls const calls;
        H::lastStarvation.clear();
        S::service();
        H::checkIn<B<12>>();
        H::checkIn<A<12>>();
        advance(10ms);
        S::service();
        for(int i = 0; i < 6; ++i) {   // a silent 60 ms: starved; b silent 60 ms: past half of 100
            advance(10ms);
            H::checkIn<Graced<12>>();
            S::service();
        }
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().check == 1);
        auto const r = H::lastStarvation.load();
        CHECK(r && r->check == 1 && r->silentMask == 0b011U && r->silentMs == 60);
    }
    test("the record is taken once");
    {
        using S           = Sup<12, B<12>, A<12>, Graced<12>>;
        auto const first  = S::takeLastStarvation();
        auto const second = S::takeLastStarvation();
        CHECK(first && first->check == 1 && !second && !H::starvationRecorded());
    }
}

// ---- properties over random loop timing ------------------------------------------------------
template<int Tag>
void property(std::uint32_t seed) {
    using S = Sup<Tag, A<Tag>, B<Tag>>;
    std::mt19937                                rng{seed};
    std::uniform_int_distribution<std::int64_t> gap{1, 40};   // a turn takes 1..40 ms
    Calls const                                 calls;
    S::service();
    // every turn both checks beat: turns up to 40 ms against windows of 50 and 100 ms never starve
    int const fedBefore = Watchdog<Tag>::feeds;
    for(int i = 0; i < 20000; ++i) {
        advance(std::chrono::milliseconds{gap(rng)});
        H::checkIn<A<Tag>>();
        H::checkIn<B<Tag>>();
        S::service();
    }
    CHECK(calls.count() == 0);
    CHECK(Watchdog<Tag>::feeds > fedBefore);
    CHECK(S::worstMs(0) == 0 && S::worstMs(1) == 0);
    // b falls silent: named after more than its 100 ms and no later than window + a period + a turn
    auto const lastBeat = FakeClock::t;
    auto       turn     = std::chrono::milliseconds{0};
    while(calls.count() == 0 && FakeClock::t - lastBeat < 1s) {
        turn = std::chrono::milliseconds{gap(rng)};
        advance(turn);
        H::checkIn<A<Tag>>();
        S::service();
    }
    auto const silent = FakeClock::t - lastBeat;
    CHECK(calls.count() == 1 && calls.last().name == "b");
    CHECK(silent > 100ms - 10ms);   // the grid is at most one period ahead of the beat
    CHECK(silent <= 100ms + 10ms + turn);
    int const fedAtStarve = Watchdog<Tag>::feeds;
    for(int i = 0; i < 100; ++i) {
        advance(std::chrono::milliseconds{gap(rng)});
        H::checkIn<A<Tag>>();
        H::checkIn<B<Tag>>();
        S::service();
    }
    CHECK(Watchdog<Tag>::feeds == fedAtStarve && calls.count() == 1);
}
}   // namespace

template<>
inline constexpr auto Kvasir::Health::injectedPolicy<> = TestPolicy{};

int main() {
    H::lastStarvation.clear();
    firstWindow();
    blockedLoop();
    suspended();
    gate();
    lifecycle();
    naming();
    Kvasir::Test::test("random loop timing: no false starvation, a silent check named in time");
    property<100>(1);
    property<101>(2);
    property<102>(20261005);
    property<103>(0xC0FFEE);
    return Kvasir::Test::report();
}
