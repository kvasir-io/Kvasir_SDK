// Kvasir::Health::Supervisor, the cases health_tests.cpp leaves out. A supervisor's state is static, so each
// scenario has its own instantiation (a local tag type on the checks, the Startup and the watchdog;
// fixtures/Health.hpp): the first window (grace + maxInterval, then maxInterval), a blocked loop counted at its real
// length, suspend / resume / startSuspended, the gated feed (only the supervisor's FeedKey feeds), two checks
// starving in one turn, the silent mask, the record taken once - and two properties over random loop timing: a
// check that beats every turn is never starved, and one that falls silent is named within one period plus one loop
// turn of its window.
#include "kvasir/Health/Health.hpp"

#include "fixtures/Health.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <string_view>
#include <type_traits>

namespace H = Kvasir::Health;
using namespace std::chrono_literals;

namespace {
using namespace Kvasir::Test::HealthFixture;

// the tag of the compile-time checks
struct Ct {};

template<typename Tag>
struct A {   // 50 ms, the default grace (= maxInterval)
    static constexpr std::string_view name        = "a";
    static constexpr auto             maxInterval = 50ms;
};

template<typename Tag>
struct B {   // 100 ms
    static constexpr std::string_view name        = "b";
    static constexpr auto             maxInterval = 100ms;
};

template<typename Tag>
struct Graced {   // 50 ms after a boot grace of 200 ms
    static constexpr std::string_view name           = "graced";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             graceAfterBoot = 200ms;
};

template<typename Tag>
struct Parked {   // starts suspended
    static constexpr std::string_view name           = "parked";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr bool             startSuspended = true;
};

// ---- the first window ------------------------------------------------------------------------
void firstWindow() {
    using Kvasir::Test::test;
    test(
      "a check that never beats: starved after grace + maxInterval (default grace = maxInterval)");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        Calls const calls;
        S::service();
        turns<S>(10);   // 100 ms = 50 + 50: not over yet
        CHECK(calls.count() == 0 && Watchdog<T>::feeds == 11);
        turns<S>(1);
        CHECK(calls.count() == 1 && calls.last().silentMs == 110 && calls.last().allowedMs == 100);
        CHECK(Watchdog<T>::feeds == 11);
        CHECK(allReported<T> == 0);
    }
    test("graceAfterBoot: the first window is 200 + 50 ms, after the first beat 50 ms");
    {
        struct T {};

        using S = Sup<T, Graced<T>>;
        Calls const calls;
        S::service();
        turns<S>(25);
        CHECK(calls.count() == 0);   // 250 ms silent: still inside the first window
        turnsBeating<S, Graced<T>>(1);
        CHECK(allReported<T> == 1);
        turns<S>(5);
        CHECK(calls.count() == 0);   // 50 ms since the beat
        turns<S>(1);
        CHECK(calls.count() == 1 && calls.last().silentMs == 60 && calls.last().allowedMs == 50);
    }
    test("graceAfterBoot: silent from boot is starved right after 250 ms");
    {
        struct T {};

        using S = Sup<T, Graced<T>>;
        Calls const calls;
        S::service();
        turns<S>(25);
        CHECK(calls.count() == 0);
        turns<S>(1);
        CHECK(calls.count() == 1 && calls.last().silentMs == 260 && calls.last().allowedMs == 250);
    }
}

// ---- a blocked loop --------------------------------------------------------------------------
void blockedLoop() {
    using Kvasir::Test::test;
    test(
      "a loop blocked 200 ms: a check that beat in the turn after it is not starved, and is fed");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        Calls const calls;
        beatOnce<S, A<T>>();
        int const fed = Watchdog<T>::feeds;
        advance(200ms);       // a flash erase, a log flood: no turn
        H::checkIn<A<T>>();   // the MainLoop hooks run before service()
        S::service();
        CHECK(calls.count() == 0 && Watchdog<T>::feeds == fed + 1);
        CHECK(S::worstMs(0) == 0);
    }
    test("a loop blocked 200 ms with the check silent: the whole 200 ms count, starved at once");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        Calls const calls;
        beatOnce<S, A<T>>();
        int const fed = Watchdog<T>::feeds;
        advance(200ms);
        S::service();
        CHECK(calls.count() == 1 && calls.last().silentMs == 200 && calls.last().allowedMs == 50);
        CHECK(Watchdog<T>::feeds == fed);
        CHECK(S::worstMs(0) == 200);
    }
    test("turns faster than the period judge nothing and feed nothing in between");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        S::service();
        int const fed = Watchdog<T>::feeds;
        for(int i = 0; i < 9; ++i) {
            advance(1ms);
            S::service();
        }
        CHECK(Watchdog<T>::feeds == fed);
        advance(1ms);
        S::service();
        CHECK(Watchdog<T>::feeds == fed + 1);
    }
}

// ---- suspend, resume, startSuspended ---------------------------------------------------------
void suspended() {
    using Kvasir::Test::test;
    test("a suspended check is not judged; resume gives it a whole window");
    {
        struct T {};

        using S = Sup<T, A<T>, B<T>>;
        Calls const calls;
        beatOnce<S, A<T>, B<T>>();
        S::suspend<A<T>>();
        turnsBeating<S, B<T>>(100);   // a second of silence from a
        CHECK(calls.count() == 0 && Watchdog<T>::feeds == 102);
        S::resume<A<T>>();
        turnsBeating<S, B<T>>(5);
        CHECK(calls.count() == 0);   // 50 ms since the resume
        turnsBeating<S, B<T>>(1);
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().silentMs == 60);
    }
    test(
      "startSuspended: not judged, not waited for by AllChecksReported; resumed, its first window");
    {
        struct T {};

        using S = Sup<T, A<T>, Parked<T>>;
        Calls const calls;
        S::service();
        turnsBeating<S, A<T>>(50);
        CHECK(calls.count() == 0 && allReported<T> == 1);
        S::resume<Parked<T>>();
        turnsBeating<S, A<T>>(10);   // grace 50 + 50
        CHECK(calls.count() == 0);
        turnsBeating<S, A<T>>(1);
        CHECK(calls.count() == 1 && calls.last().name == "parked" && calls.last().check == 1);
        CHECK(allReported<T> == 1);
    }
    test("a beat while suspended is no starvation and no stale silence after the resume");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        Calls const calls;
        beatOnce<S, A<T>>();
        turns<S>(4);   // 40 ms silent, then suspended
        S::suspend<A<T>>();
        advance(500ms);
        S::service();
        S::resume<A<T>>();
        turns<S>(5);   // the 40 ms from before the suspension do not count
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
static_assert(!PlainFeed<GatedWatchdog<Ct>> && !BracedFeed<GatedWatchdog<Ct>>);

void gate() {
    using Kvasir::Test::test;
    test(
      "a gated watchdog (feed takes a FeedKey) is fed by the supervisor, and only while healthy");

    struct T {};

    using S = H::Supervisor<Startup<T, A<T>>, GatedWatchdog<T>, FakeClock, Period10>;
    Calls const calls;
    S::service();
    turnsBeating<S, A<T>>(20);
    CHECK(GatedWatchdog<T>::feeds == 21 && calls.count() == 0);
    turns<S>(20);
    CHECK(calls.count() == 1);
    CHECK(GatedWatchdog<T>::feeds == 21 + 5);   // the five turns inside the window, none after
}

// ---- the watchdog's life: holdOff, armed at the first turn ------------------------------------
// a gated watchdog as chip_rp_common's: feed, arm and disarm all take the key
template<typename Tag>
struct OwnedWatchdog {
    static constexpr auto              timeout = std::chrono::milliseconds{500};
    [[maybe_unused]] static inline int feeds   = 0;
    [[maybe_unused]] static inline int arms    = 0;
    [[maybe_unused]] static inline int disarms = 0;

    static void feed(H::FeedKey) { ++feeds; }

    static void arm(H::FeedKey) { ++arms; }

    static void disarm(H::FeedKey) { ++disarms; }
};

// an ungated one with the same life
template<typename Tag>
struct PlainWatchdog {
    static constexpr auto              timeout = std::chrono::milliseconds{500};
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
static_assert(!PlainArm<OwnedWatchdog<Ct>> && !PlainDisarm<OwnedWatchdog<Ct>>
              && !PlainFeed<OwnedWatchdog<Ct>>);

template<typename W,
         typename Tag>
void lifeOf() {
    using S = H::Supervisor<Startup<Tag, A<Tag>>, W, FakeClock, Period10>;
    S::holdOff();
    CHECK(W::disarms == 1 && W::arms == 0 && W::feeds == 0);
    advance(700ms);   // a boot longer than the watchdog's timeout: nothing runs meanwhile
    S::service();     // the first turn arms, which loads the whole timeout
    CHECK(W::arms == 1 && W::feeds == 0);
    turnsBeating<S, A<Tag>>(5);
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
static_assert(HoldsOff<H::Supervisor<Startup<Ct,
                                             A<Ct>>,
                                     OwnedWatchdog<Ct>,
                                     FakeClock,
                                     Period10>>);
static_assert(HoldsOff<H::Supervisor<Startup<Ct,
                                             A<Ct>>,
                                     PlainWatchdog<Ct>,
                                     FakeClock,
                                     Period10>>);
static_assert(!HoldsOff<H::Supervisor<Startup<Ct,
                                              A<Ct>>,
                                      GatedWatchdog<Ct>,
                                      FakeClock,
                                      Period10>>);

void lifecycle() {
    using Kvasir::Test::test;
    test(
      "a gated watchdog: holdOff stops it before the first turn only, the first turn arms, then "
      "feeds");

    struct Owned {};

    lifeOf<OwnedWatchdog<Owned>, Owned>();
    test("an ungated watchdog with arm()/disarm(): the same life through the plain calls");

    struct Plain {};

    lifeOf<PlainWatchdog<Plain>, Plain>();
    test("a watchdog without arm(): the first turn feeds");
    {
        struct T {};

        using S = Sup<T, A<T>>;
        S::service();
        CHECK(Watchdog<T>::feeds == 1);
    }
}

// ---- which check, the mask, the record -------------------------------------------------------
void naming() {
    using Kvasir::Test::test;
    test(
      "two checks over their windows in one turn: the first in the list is named, the policy runs "
      "once");
    {
        struct T {};

        using S = Sup<T, A<T>, B<T>>;
        Calls const calls;
        H::lastStarvation.clear();
        beatOnce<S, A<T>, B<T>>();
        advance(300ms);   // both far over
        S::service();
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().check == 0);
        auto const r = H::lastStarvation.load();
        CHECK(r && r->check == 0 && r->nameHash == H::fnv1a("a") && r->silentMask == 0b11U);
        advance(300ms);
        S::service();
        CHECK(calls.count() == 1);   // latched: b is not reported on top
    }

    // the next two cases share one supervisor
    struct T {};

    using S = Sup<T, B<T>, A<T>, Graced<T>>;   // b 100 ms, a 50 ms, graced beating
    test("the silent mask: checks past HALF their window, a beating one not");
    {
        Calls const calls;
        H::lastStarvation.clear();
        beatOnce<S, B<T>, A<T>>();
        turnsBeating<S, Graced<T>>(
          6);   // a silent 60 ms: starved; b silent 60 ms: past half of 100
        CHECK(calls.count() == 1 && calls.last().name == "a" && calls.last().check == 1);
        auto const r = H::lastStarvation.load();
        CHECK(r && r->check == 1 && r->silentMask == 0b011U && r->silentMs == 60);
    }
    test("the record is taken once");
    {
        auto const first  = S::takeLastStarvation();
        auto const second = S::takeLastStarvation();
        CHECK(first && first->check == 1 && !second && !H::starvationRecorded());
    }
}

// ---- properties over random loop timing ------------------------------------------------------
template<typename Tag>
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
    auto const lastBeat = FakeClock::now();
    auto       turn     = std::chrono::milliseconds{0};
    while(calls.count() == 0 && FakeClock::now() - lastBeat < 1s) {
        turn = std::chrono::milliseconds{gap(rng)};
        advance(turn);
        H::checkIn<A<Tag>>();
        S::service();
    }
    auto const silent = FakeClock::now() - lastBeat;
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
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::HealthFixture::TestPolicy;
};

int main() {
    H::lastStarvation.clear();
    firstWindow();
    blockedLoop();
    suspended();
    gate();
    lifecycle();
    naming();
    Kvasir::Test::test("random loop timing: no false starvation, a silent check named in time");

    struct P1 {};

    struct P2 {};

    struct P3 {};

    struct P4 {};

    property<P1>(1);
    property<P2>(2);
    property<P3>(20261005);
    property<P4>(0xC0FFEE);
    return Kvasir::Test::report();
}
