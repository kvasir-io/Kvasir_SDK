// Kvasir::Health's local recovery (a check Config's onStarved): the call at the first period past the window, the
// watchdog fed through the recovery window, a beat ending the recovery and resetting the ladder, every rung used
// before the policy, Recovery::cannot escalating at once, the four signatures, the budget, the "KHRV" record (the
// rung in progress during the call), one onStarved per turn, a check without one escalating while another recovers,
// suspend/resume, and the worst case from the last beat to the escalation; deferRecoveries (request, run, cancel,
// refusal, service() inside onStarved, latched policy, no onStarved). A supervisor's state is static: one
// instantiation per scenario (a tag on the checks, the Startup and the watchdog).
#include "kvasir/Util/Health.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
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

struct Seen {
    static inline std::vector<H::Starved> all;
};

struct TestPolicy {
    static constexpr bool keepFeeding = false;

    static void operator()(H::Starved const& s) { Seen::all.push_back(s); }
};

template<int Tag>
struct Watchdog {
    static constexpr auto              Timeout = std::chrono::milliseconds{500};
    [[maybe_unused]] static inline int feeds   = 0;

    static void feed() { ++feeds; }
};

template<int Tag, typename... Cs>
struct Startup {
    template<typename Hook>
    using ExtendsOn = brigand::list<H::Check<Cs>...>;

    template<typename Hook>
    static void run() {}
};

struct Period10 {
    static constexpr auto period = 10ms;
};

template<int Tag, typename... Cs>
using Sup = H::Supervisor<Startup<Tag, Cs...>, Watchdog<Tag>, FakeClock, Period10>;

void advance(std::chrono::milliseconds d) { FakeClock::t += d; }

// what each onStarved saw
struct Call {
    int           tag;
    std::uint8_t  n;
    std::uint8_t  of;
    std::uint32_t inProgress;   // the KHRV record's word during the call
};

std::vector<Call> calls;

template<int Tag>
int callsOf() {
    int c = 0;
    for(auto const& x : calls) {
        if(x.tag == Tag) { ++c; }
    }
    return c;
}

std::uint32_t inProgressNow() {
    auto const r = H::lastRecovery.load();
    return r ? r->inProgress : 0xDEADU;
}

// 50 ms window, two rungs of 30 ms; what onStarved returns and whether it takes 5 ms is per tag
template<int Tag>
struct Ladder {
    static constexpr std::string_view name             = "ladder";
    static constexpr auto             maxInterval      = 50ms;
    static constexpr auto             graceAfterBoot   = 0ms;
    static constexpr auto             recoveryAttempts = 2;
    static constexpr auto             recoveryWindow   = 30ms;
    static inline H::Recovery         answer           = H::Recovery::started;
    static inline bool                slow             = false;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({Tag, a.n, a.of, inProgressNow()});
        if(slow) { advance(5ms); }
        return answer;
    }
};

// the defaults: one rung, window = maxInterval; plain void()
template<int Tag>
struct PlainVoid {
    static constexpr std::string_view name           = "plain";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static void onStarved() { calls.push_back({Tag, 0, 0, inProgressNow()}); }
};

template<int Tag>
struct PlainRecovery {
    static constexpr std::string_view name           = "plainR";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static H::Recovery onStarved() {
        calls.push_back({Tag, 0, 0, inProgressNow()});
        return H::Recovery::started;
    }
};

template<int Tag>
struct VoidAttempt {
    static constexpr std::string_view name           = "voidA";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static void onStarved(H::Attempt a) { calls.push_back({Tag, a.n, a.of, inProgressNow()}); }
};

// no onStarved
template<int Tag>
struct Plain {
    static constexpr std::string_view name           = "bare";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             graceAfterBoot = 0ms;
};

// a second ladder check, for two in one turn
template<int Tag>
struct Other {
    static constexpr std::string_view name             = "other";
    static constexpr auto             maxInterval      = 50ms;
    static constexpr auto             graceAfterBoot   = 0ms;
    static constexpr auto             recoveryAttempts = 1;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({Tag + 1000, a.n, a.of, inProgressNow()});
        return H::Recovery::started;
    }
};

// silent turns of 10 ms until the policy or `max` turns; returns the turns taken
template<typename S>
int silentUntilPolicy(int max) {
    std::size_t const before = Seen::all.size();
    for(int i = 1; i <= max; ++i) {
        advance(10ms);
        S::service();
        if(Seen::all.size() != before) { return i; }
    }
    return -1;
}

// ---- compile time ---------------------------------------------------------------------------------------------
using NoRecovery = H::Detail::Configs<brigand::list<H::Check<Plain<0>>>>;
static_assert(!NoRecovery::anyRecovery && NoRecovery::attempts[0] == 0);
static_assert(sizeof(H::Detail::RecoveryState<1,
                                              false>)
              == 1);
using WithRecovery = H::Detail::Configs<
  brigand::list<H::Check<Plain<0>>, H::Check<Ladder<0>>, H::Check<PlainVoid<0>>>>;
static_assert(WithRecovery::anyRecovery);
static_assert(WithRecovery::attempts[0] == 0 && WithRecovery::attempts[1] == 2
              && WithRecovery::attempts[2] == 1);
static_assert(WithRecovery::recoveryWindowMs[1] == 30 && WithRecovery::recoveryWindowMs[2] == 40);
static_assert(H::Recovers<PlainVoid<0>> && H::Recovers<PlainRecovery<0>>
              && H::Recovers<VoidAttempt<0>> && H::Recovers<Ladder<0>> && !H::Recovers<Plain<0>>);
static_assert(H::LadderRecovery<VoidAttempt<0>> && !H::LadderRecovery<PlainVoid<0>>);
// 50 + 2 x 30 + 3 x 10 + 500
static_assert(Sup<0,
                  Ladder<0>>::worstToEscalationMs<Ladder<0>>()
              == 640);
// without onStarved: the window, one period, the timeout
static_assert(Sup<0,
                  Plain<0>>::worstToEscalationMs<Plain<0>>()
              == 50 + 10 + 500);
static_assert(H::recoveryInProgress(3,
                                    2)
                == (4U | (2U << 16U))
              && H::inProgressCheck(H::recoveryInProgress(3,
                                                          2))
                   == 3
              && H::inProgressAttempt(H::recoveryInProgress(3,
                                                            2))
                   == 2);

// ---- scenarios ------------------------------------------------------------------------------------------------
void recoverOnce() {
    using Kvasir::Test::test;
    test(
      "onStarved at the first period past the window; fed through the recovery window; a beat "
      "recovers");
    using L = Ladder<1>;
    using S = Sup<1, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();   // the beat: the steady 50 ms window
    std::size_t const policy = Seen::all.size();
    for(int i = 0; i < 5; ++i) {   // 50 ms silent: not over
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<1>() == 0);
    advance(10ms);
    S::service();   // 60 ms: rung 1
    CHECK(callsOf<1>() == 1 && calls.back().n == 0 && calls.back().of == 2);
    CHECK(calls.back().inProgress == H::recoveryInProgress(0, 1));
    CHECK(inProgressNow() == 0);
    CHECK(S::attemptOf(0) == 1);
    int const fed = Watchdog<1>::feeds;
    for(int i = 0; i < 2; ++i) {   // inside the 30 ms recovery window: fed, no rung
        advance(10ms);
        S::service();
    }
    CHECK(Watchdog<1>::feeds == fed + 2 && callsOf<1>() == 1);
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0 && Seen::all.size() == policy);
    auto const r = H::lastRecovery.load();
    CHECK(r && r->total == 1 && r->lastCheck == 0 && r->lastNameHash == H::fnv1a("ladder")
          && r->inProgress == 0 && r->escalatedAttempts == 0
          && r->lastUptimeMs
               == static_cast<std::uint32_t>(FakeClock::t.time_since_epoch().count()));

    test("the ladder starts over after a beat: the next silence gets rung 1 again");
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<1>() == 2 && calls.back().n == 0);
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(S::recoveredCount(0) == 2 && H::lastRecovery.load()->total == 2);
}

void neverBeats() {
    using Kvasir::Test::test;
    test(
      "never beating: both rungs, each after its window, then the policy once and no more feeds");
    using L = Ladder<2>;
    using S = Sup<2, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    auto const lastBeat = FakeClock::t;
    int const  turns    = silentUntilPolicy<S>(100);
    CHECK(callsOf<2>() == 2 && calls[calls.size() - 2].n == 0 && calls.back().n == 1);
    // 60 ms to rung 1, 40 to rung 2 (30 ms window + one period), 40 to the policy
    CHECK(turns == 14);
    CHECK(static_cast<std::uint32_t>((FakeClock::t - lastBeat).count())
          == S::worstToEscalationMs<L>() - S::timeoutMs);
    CHECK(Seen::all.back().name == "ladder" && Seen::all.back().silentMs == 40
          && Seen::all.back().allowedMs == 30);
    CHECK(H::lastRecovery.load()->escalatedAttempts == 2 && H::lastRecovery.load()->total == 0);
    CHECK(H::lastStarvation.load()->check == 0);
    int const fed = Watchdog<2>::feeds;
    for(int i = 0; i < 10; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(Watchdog<2>::feeds == fed && callsOf<2>() == 2);
    H::lastStarvation.clear();
}

void cannot() {
    using Kvasir::Test::test;
    test("Recovery::cannot on rung 1: the policy in the same turn, no recovery window");
    using L   = Ladder<3>;
    using S   = Sup<3, L>;
    L::answer = H::Recovery::cannot;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    int const turns = silentUntilPolicy<S>(100);
    CHECK(turns == 6 && callsOf<3>() == 1);
    CHECK(H::lastRecovery.load()->escalatedAttempts == 2);   // the ladder is spent
    H::lastStarvation.clear();
}

void signatures() {
    using Kvasir::Test::test;
    test(
      "the four forms: void(), Recovery(), void(Attempt), Recovery(Attempt); defaults 1 rung, "
      "window = maxInterval");
    using S                  = Sup<4, PlainVoid<4>, PlainRecovery<5>, VoidAttempt<6>>;
    std::size_t const policy = Seen::all.size();
    S::service();
    H::checkIn<PlainVoid<4>>();
    H::checkIn<PlainRecovery<5>>();
    H::checkIn<VoidAttempt<6>>();
    advance(10ms);
    S::service();
    for(int i = 0; i < 7; ++i) {   // 50 ms > 40: one per turn, in list order
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<4>() == 1 && callsOf<5>() == 1 && callsOf<6>() == 1);
    CHECK(calls.back().tag == 6 && calls.back().n == 0 && calls.back().of == 1);
    H::checkIn<PlainVoid<4>>();
    H::checkIn<PlainRecovery<5>>();
    H::checkIn<VoidAttempt<6>>();
    advance(10ms);
    S::service();
    CHECK(S::recoveredCount(0) == 1 && S::recoveredCount(1) == 1 && S::recoveredCount(2) == 1);
    CHECK(Seen::all.size() == policy);
}

void budget() {
    using Kvasir::Test::test;
    test("an onStarved over the budget (5 ms > 2 ms): counted, the recovery goes on");
    using L = Ladder<7>;
    using S = Sup<7, L>;
    L::slow = true;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<7>() == 1 && S::recoveryOverruns() == 1);
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(S::recoveredCount(0) == 1);
}

void twoInOneTurn() {
    using Kvasir::Test::test;
    test(
      "two checks past their window in one turn: one onStarved per turn, the second the turn "
      "after");
    using S = Sup<8, Ladder<8>, Other<8>>;
    S::service();
    H::checkIn<Ladder<8>>();
    H::checkIn<Other<8>>();
    advance(10ms);
    S::service();
    std::size_t const policy = Seen::all.size();
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<8>() == 1 && callsOf<1008>() == 0 && Seen::all.size() == policy);
    advance(10ms);
    S::service();
    CHECK(callsOf<1008>() == 1 && Seen::all.size() == policy);
}

void bareEscalatesWhileOtherRecovers() {
    using Kvasir::Test::test;
    test(
      "a check without onStarved escalates at once while another is in its recovery window; then "
      "no more rungs");
    using S = Sup<9, Ladder<9>, Plain<9>>;
    S::service();
    H::checkIn<Ladder<9>>();
    advance(10ms);
    S::service();
    std::size_t const policy = Seen::all.size();
    // the ladder's rung 1 at turn 6 (window 30 ms, rung 2 would be turn 10); "bare" beats twice, then starves at
    // turn 8 - inside the ladder's recovery window
    for(int i = 1; i <= 8; ++i) {
        if(i <= 2) { H::checkIn<Plain<9>>(); }
        advance(10ms);
        S::service();
        if(i == 7) { CHECK(callsOf<9>() == 1 && Seen::all.size() == policy); }
    }
    CHECK(Seen::all.size() == policy + 1 && Seen::all.back().name == "bare");
    int const fed = Watchdog<9>::feeds;
    for(int i = 0; i < 10; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<9>() == 1 && Watchdog<9>::feeds == fed);   // latched: no rung, no feed
    H::lastStarvation.clear();
}

void suspendDuringRecovery() {
    using Kvasir::Test::test;
    test("suspended in its recovery window: no rung while suspended; resume starts a fresh ladder");
    using L = Ladder<10>;
    using S = Sup<10, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<10>() == 1 && S::attemptOf(0) == 1);
    S::suspend<L>();
    for(int i = 0; i < 20; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<10>() == 1);
    S::resume<L>();
    CHECK(S::attemptOf(0) == 0);
    for(int i = 0; i < 5; ++i) {   // the full 50 ms window again, not the 30 ms recovery window
        advance(10ms);
        S::service();
    }
    CHECK(callsOf<10>() == 1);
    advance(10ms);
    S::service();
    CHECK(callsOf<10>() == 2 && calls.back().n == 0);
}

void recordAcrossBoots() {
    using Kvasir::Test::test;
    test(
      "a reset inside onStarved: the record names the check and the rung; starvedCheck() finds it");
    H::lastStarvation.clear();
    H::lastRecovery.store(H::Recoveries{.total             = 3,
                                        .lastCheck         = 0,
                                        .lastNameHash      = H::fnv1a("ladder"),
                                        .lastUptimeMs      = 1234,
                                        .inProgress        = H::recoveryInProgress(0, 2),
                                        .escalatedAttempts = 0});
    CHECK(H::starvedCheck() == 0U);
    using S       = Sup<11, Ladder<11>>;
    auto const r1 = S::takeLastRecoveries();
    CHECK(r1 && r1->total == 3 && H::inProgressAttempt(r1->inProgress) == 2);
    CHECK(!S::takeLastRecoveries() && !H::starvedCheck());

    test("the first service() starts this run's record");
    H::lastRecovery.store(H::Recoveries{.total             = 9,
                                        .lastCheck         = 0,
                                        .lastNameHash      = 0,
                                        .lastUptimeMs      = 0,
                                        .inProgress        = 0,
                                        .escalatedAttempts = 0});
    S::service();
    CHECK(H::lastRecovery.load() && H::lastRecovery.load()->total == 0);
}

// ---- deferRecoveries: service() from an interrupt, onStarved from the main loop ------------------------------
struct Deferred10 {
    static constexpr auto period          = 10ms;
    static constexpr bool deferRecoveries = true;
};

template<int Tag, typename... Cs>
using DSup = H::Supervisor<Startup<Tag, Cs...>, Watchdog<Tag>, FakeClock, Deferred10>;

// the watchdog sees no onStarved: a budget past its timeout is allowed (500 ms here)
struct DeferredSlowBudget {
    static constexpr auto period          = 10ms;
    static constexpr bool deferRecoveries = true;
    static constexpr auto recoveryBudget  = 600ms;
};

static_assert(DSup<20,
                   Ladder<20>>::deferRecoveries
              && !Sup<20,
                      Ladder<20>>::deferRecoveries);
static_assert(H::Supervisor<Startup<29,
                                    Ladder<29>>,
                            Watchdog<29>,
                            FakeClock,
                            DeferredSlowBudget>::budgetBelowTimeout);
// the window and the rungs do not change: the worst case holds as it is
static_assert(DSup<20,
                   Ladder<20>>::worstToEscalationMs<Ladder<20>>()
              == 640);

// a ladder check whose onStarved runs a hook (a service() preempting it)
template<int Tag>
struct Hooked {
    static constexpr std::string_view   name             = "hooked";
    static constexpr auto               maxInterval      = 50ms;
    static constexpr auto               graceAfterBoot   = 0ms;
    static constexpr auto               recoveryAttempts = 2;
    static constexpr auto               recoveryWindow   = 30ms;
    static inline std::function<void()> during{};
    static inline H::Recovery           answer = H::Recovery::started;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({Tag, a.n, a.of, inProgressNow()});
        if(during) { during(); }
        return answer;
    }
};

// silent 10 ms turns until a recovery is requested; the turns taken, -1 if none in `max`
template<typename S>
int silentUntilRequested(int max) {
    for(int i = 1; i <= max; ++i) {
        advance(10ms);
        S::service();
        if(S::pendingRecoveries() != 0) { return i; }
    }
    return -1;
}

void deferredRequestRunRecover() {
    using Kvasir::Test::test;
    test("deferred: service() requests at the first period past the window, runRecoveries() calls");
    using L = Ladder<20>;
    using S = DSup<20, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    std::size_t const policy = Seen::all.size();
    CHECK(silentUntilRequested<S>(10) == 6);   // 60 ms silent, as without deferRecoveries
    CHECK(callsOf<20>() == 0 && S::attemptOf(0) == 1 && inProgressNow() == 0);
    CHECK(S::runRecoveries());
    CHECK(callsOf<20>() == 1 && calls.back().n == 0 && calls.back().of == 2);
    CHECK(calls.back().inProgress == H::recoveryInProgress(0, 1));
    CHECK(inProgressNow() == 0 && S::pendingRecoveries() == 0);
    CHECK(!S::runRecoveries() && callsOf<20>() == 1);

    test("deferred: fed through the window, a beat recovers");
    int const fed = Watchdog<20>::feeds;
    advance(10ms);
    S::service();
    CHECK(Watchdog<20>::feeds == fed + 1);
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0 && Seen::all.size() == policy);
    CHECK(H::lastRecovery.load() && H::lastRecovery.load()->total == 1);
}

void deferredNeverRun() {
    using Kvasir::Test::test;
    test("deferred: a loop that never runs the requests escalates after the last rung's window");
    using L = Ladder<21>;
    using S = DSup<21, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    std::size_t const before = Seen::all.size();
    int const         turns  = silentUntilPolicy<S>(30);
    CHECK(turns > 0 && turns * 10 + 10 <= static_cast<int>(S::worstToEscalationMs<L>()));
    CHECK(Seen::all.size() == before + 1 && Seen::all.back().check == 0);
    CHECK(callsOf<21>() == 0 && S::attemptOf(0) == 2);
    auto const r = H::lastRecovery.load();
    CHECK(r && r->escalatedAttempts == 2 && r->inProgress == 0);
}

void deferredCannot() {
    using Kvasir::Test::test;
    test("deferred: Recovery::cannot escalates at the next service()");
    using L   = Ladder<22>;
    using S   = DSup<22, L>;
    L::answer = H::Recovery::cannot;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(silentUntilRequested<S>(10) == 6);
    std::size_t const before = Seen::all.size();
    CHECK(S::runRecoveries() && callsOf<22>() == 1);
    CHECK(Seen::all.size() == before);
    advance(10ms);
    S::service();
    CHECK(Seen::all.size() == before + 1 && Seen::all.back().check == 0);
    // named with the silence that led to the request (60 ms of 50) and the 10 ms since, as without deferRecoveries
    CHECK(Seen::all.back().silentMs == 70 && Seen::all.back().allowedMs == 50);
    CHECK(callsOf<22>() == 1);
}

void deferredBeatCancels() {
    using Kvasir::Test::test;
    test("deferred: a beat before the loop got to the request cancels it");
    using L = Ladder<23>;
    using S = DSup<23, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(silentUntilRequested<S>(10) == 6);
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(S::pendingRecoveries() == 0 && !S::runRecoveries() && callsOf<23>() == 0);
    CHECK(S::attemptOf(0) == 0 && S::recoveredCount(0) == 1);
}

void deferredServiceInsideOnStarved() {
    using Kvasir::Test::test;
    test("deferred: service() preempting onStarved (the check beats): the record ends consistent");
    using L = Hooked<24>;
    using S = DSup<24, L>;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(silentUntilRequested<S>(10) == 6);
    L::during = [] {
        H::checkIn<L>();   // the subsystem came back
        advance(10ms);
        S::service();   // the interrupt: recovered(), its own record store
    };
    CHECK(S::runRecoveries());
    L::during = {};
    CHECK(calls.back().inProgress == H::recoveryInProgress(0, 1));
    auto const r = H::lastRecovery.load();
    CHECK(r && r->total == 1 && r->inProgress == 0);
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0 && S::pendingRecoveries() == 0);

    test("deferred: a second rung requested while the first never ran: the newest rung runs once");
    for(int i = 0; i < 6; ++i) {
        advance(10ms);
        S::service();
    }
    CHECK(S::attemptOf(0) == 1);
    for(int i = 0; i < 4; ++i) {   // past the 30 ms window: rung 2 replaces rung 1
        advance(10ms);
        S::service();
    }
    CHECK(S::attemptOf(0) == 2 && S::pendingRecoveries() == 1);
    int const before = callsOf<24>();
    CHECK(S::runRecoveries() && !S::runRecoveries());
    CHECK(callsOf<24>() == before + 1 && calls.back().n == 1);
}

void deferredCannotAfterBeat() {
    using Kvasir::Test::test;
    test("deferred: Recovery::cannot from a check that beat during the call does not escalate it");
    using L   = Hooked<25>;
    using S   = DSup<25, L>;
    L::answer = H::Recovery::cannot;
    S::service();
    H::checkIn<L>();
    advance(10ms);
    S::service();
    CHECK(silentUntilRequested<S>(10) == 6);
    L::during = [] {
        H::checkIn<L>();   // the subsystem came back, then onStarved says it has no lever
        advance(10ms);
        S::service();
    };
    std::size_t const before = Seen::all.size();
    CHECK(S::runRecoveries());
    L::during = {};
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0);
    for(int i = 0; i < 4; ++i) {   // silent turns inside its 50 ms window: no stale refusal left
        advance(10ms);
        S::service();
    }
    CHECK(Seen::all.size() == before && callsOf<25>() == 1 && S::pendingRecoveries() == 0);
}

void deferredLatchDropsRequests() {
    using Kvasir::Test::test;
    test("deferred: a policy that stops feeding drops the requests the loop has not run");
    using S = DSup<26, Ladder<26>, Plain<26>>;
    S::service();
    H::checkIn<Ladder<26>>();
    advance(10ms);
    S::service();
    std::size_t const policy = Seen::all.size();
    // as bareEscalatesWhileOtherRecovers: the ladder's request at turn 6, "bare" starves at turn 8
    for(int i = 1; i <= 8; ++i) {
        if(i <= 2) { H::checkIn<Plain<26>>(); }
        advance(10ms);
        S::service();
        if(i == 7) { CHECK(S::pendingRecoveries() == 1U && Seen::all.size() == policy); }
    }
    CHECK(Seen::all.size() == policy + 1 && Seen::all.back().name == "bare");
    CHECK(S::pendingRecoveries() == 0 && !S::runRecoveries() && callsOf<26>() == 0);
    H::lastStarvation.clear();
}

void deferredWithoutOnStarved() {
    using Kvasir::Test::test;
    test(
      "deferred: an image without onStarved keeps runRecoveries() in its main loop, which does "
      "nothing");
    using S = DSup<27, Plain<27>>;
    static_assert(S::deferRecoveries && !S::Cfg::anyRecovery);
    S::service();
    CHECK(!S::runRecoveries() && S::pendingRecoveries() == 0);
}
}   // namespace

template<>
inline constexpr auto Kvasir::Health::injectedPolicy<> = TestPolicy{};

int main() {
    H::lastStarvation.clear();
    H::lastRecovery.clear();
    recoverOnce();
    neverBeats();
    cannot();
    signatures();
    budget();
    twoInOneTurn();
    bareEscalatesWhileOtherRecovers();
    suspendDuringRecovery();
    recordAcrossBoots();
    deferredRequestRunRecover();
    deferredNeverRun();
    deferredCannot();
    deferredBeatCancels();
    deferredServiceInsideOnStarved();
    deferredCannotAfterBeat();
    deferredLatchDropsRequests();
    deferredWithoutOnStarved();
    return Kvasir::Test::report();
}
