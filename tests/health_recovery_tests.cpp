// Kvasir::Health's local recovery (a check Config's onStarved): the call at the first period past the window, the
// watchdog fed through the recovery window, a beat ending the recovery and resetting the ladder, every rung used
// before the policy, Recovery::cannot escalating at once, the four signatures, the budget, the "KHRV" record (the
// rung in progress during the call), one onStarved per turn, a check without one escalating while another recovers,
// suspend/resume, and the worst case from the last beat to the escalation; deferRecoveries (request, run, cancel,
// refusal, service() inside onStarved, latched policy, no onStarved). A supervisor's state is static: one
// instantiation per scenario (a local tag type on the checks, the Startup and the watchdog; fixtures/Health.hpp).
#include "kvasir/Health/Health.hpp"

#include "fixtures/Health.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace H = Kvasir::Health;
using namespace std::chrono_literals;

namespace {
using namespace Kvasir::Test::HealthFixture;

// the tag of the compile-time checks
struct Ct {};

// one distinct id per tag type: the address of a variable of its own
template<typename Tag>
inline constexpr char tagAnchor = 0;

template<typename Tag>
void const* tagId() {
    return &tagAnchor<Tag>;
}

// what each onStarved saw
struct Call {
    void const*   tag;
    std::uint8_t  n;
    std::uint8_t  of;
    std::uint32_t inProgress;   // the KHRV record's word during the call
};

std::vector<Call> calls;

template<typename Tag>
int callsOf() {
    int c = 0;
    for(auto const& x : calls) {
        if(x.tag == tagId<Tag>()) { ++c; }
    }
    return c;
}

std::uint32_t inProgressNow() {
    auto const r = H::lastRecovery.load();
    return r ? r->inProgress : 0xDEADU;
}

// 50 ms window, two rungs of 30 ms; what onStarved returns and whether it takes 5 ms is per tag
template<typename Tag>
struct Ladder {
    static constexpr std::string_view name             = "ladder";
    static constexpr auto             maxInterval      = 50ms;
    static constexpr auto             graceAfterBoot   = 0ms;
    static constexpr auto             recoveryAttempts = 2;
    static constexpr auto             recoveryWindow   = 30ms;
    static inline H::Recovery         answer           = H::Recovery::started;
    static inline bool                slow             = false;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({tagId<Tag>(), a.n, a.of, inProgressNow()});
        if(slow) { advance(5ms); }
        return answer;
    }
};

// the defaults: one rung, window = maxInterval; plain void()
template<typename Tag>
struct PlainVoid {
    static constexpr std::string_view name           = "plain";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static void onStarved() { calls.push_back({tagId<Tag>(), 0, 0, inProgressNow()}); }
};

template<typename Tag>
struct PlainRecovery {
    static constexpr std::string_view name           = "plainR";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static H::Recovery onStarved() {
        calls.push_back({tagId<Tag>(), 0, 0, inProgressNow()});
        return H::Recovery::started;
    }
};

template<typename Tag>
struct VoidAttempt {
    static constexpr std::string_view name           = "voidA";
    static constexpr auto             maxInterval    = 40ms;
    static constexpr auto             graceAfterBoot = 0ms;

    static void onStarved(H::Attempt a) {
        calls.push_back({tagId<Tag>(), a.n, a.of, inProgressNow()});
    }
};

// no onStarved
template<typename Tag>
struct Plain {
    static constexpr std::string_view name           = "bare";
    static constexpr auto             maxInterval    = 50ms;
    static constexpr auto             graceAfterBoot = 0ms;
};

// a second ladder check, for two in one turn; its calls count as callsOf<Other<Tag>>
template<typename Tag>
struct Other {
    static constexpr std::string_view name             = "other";
    static constexpr auto             maxInterval      = 50ms;
    static constexpr auto             graceAfterBoot   = 0ms;
    static constexpr auto             recoveryAttempts = 1;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({tagId<Other>(), a.n, a.of, inProgressNow()});
        return H::Recovery::started;
    }
};

// silent turns of 10 ms until the policy runs; the turns taken, -1 if not within max
template<typename S>
int silentUntilPolicy(int max) {
    std::size_t const before = Seen::all.size();
    return turnsUntil<S>(max, [before] { return Seen::all.size() != before; });
}

// silent turns of 10 ms until a recovery is requested; the turns taken, -1 if none within max
template<typename S>
int silentUntilRequested(int max) {
    return turnsUntil<S>(max, [] { return S::pendingRecoveries() != 0; });
}

// ---- compile time ---------------------------------------------------------------------------------------------
using NoRecovery = H::Detail::Configs<brigand::list<H::Check<Plain<Ct>>>>;
static_assert(!NoRecovery::anyRecovery && NoRecovery::attempts[0] == 0);
static_assert(sizeof(H::Detail::RecoveryState<1,
                                              false>)
              == 1);
using WithRecovery = H::Detail::Configs<
  brigand::list<H::Check<Plain<Ct>>, H::Check<Ladder<Ct>>, H::Check<PlainVoid<Ct>>>>;
static_assert(WithRecovery::anyRecovery);
static_assert(WithRecovery::attempts[0] == 0 && WithRecovery::attempts[1] == 2
              && WithRecovery::attempts[2] == 1);
static_assert(WithRecovery::recoveryWindowMs[1] == 30 && WithRecovery::recoveryWindowMs[2] == 40);
static_assert(H::Recovers<PlainVoid<Ct>> && H::Recovers<PlainRecovery<Ct>>
              && H::Recovers<VoidAttempt<Ct>> && H::Recovers<Ladder<Ct>>
              && !H::Recovers<Plain<Ct>>);
static_assert(H::LadderRecovery<VoidAttempt<Ct>> && !H::LadderRecovery<PlainVoid<Ct>>);
// 50 + 2 x 30 + 3 x 10 + 500
static_assert(Sup<Ct,
                  Ladder<Ct>>::worstToEscalationMs<Ladder<Ct>>()
              == 640);
// without onStarved: the window, one period, the timeout
static_assert(Sup<Ct,
                  Plain<Ct>>::worstToEscalationMs<Plain<Ct>>()
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

    struct T {};

    using L = Ladder<T>;
    using S = Sup<T, L>;
    beatOnce<S, L>();   // the beat: the steady 50 ms window
    std::size_t const policy = Seen::all.size();
    turns<S>(5);   // 50 ms silent: not over
    CHECK(callsOf<T>() == 0);
    turns<S>(1);   // 60 ms: rung 1
    CHECK(callsOf<T>() == 1 && calls.back().n == 0 && calls.back().of == 2);
    CHECK(calls.back().inProgress == H::recoveryInProgress(0, 1));
    CHECK(inProgressNow() == 0);
    CHECK(S::attemptOf(0) == 1);
    int const fed = Watchdog<T>::feeds;
    turns<S>(2);   // inside the 30 ms recovery window: fed, no rung
    CHECK(Watchdog<T>::feeds == fed + 2 && callsOf<T>() == 1);
    turnsBeating<S, L>(1);
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0 && Seen::all.size() == policy);
    auto const r = H::lastRecovery.load();
    CHECK(r && r->total == 1 && r->lastCheck == 0 && r->lastNameHash == H::fnv1a("ladder")
          && r->inProgress == 0 && r->escalatedAttempts == 0
          && r->lastUptimeMs
               == static_cast<std::uint32_t>(FakeClock::now().time_since_epoch().count()));

    test("the ladder starts over after a beat: the next silence gets rung 1 again");
    turns<S>(6);
    CHECK(callsOf<T>() == 2 && calls.back().n == 0);
    turnsBeating<S, L>(1);
    auto const again = H::lastRecovery.load();
    CHECK(S::recoveredCount(0) == 2 && again && again->total == 2);
}

void neverBeats() {
    using Kvasir::Test::test;
    test(
      "never beating: both rungs, each after its window, then the policy once and no more feeds");

    struct T {};

    using L = Ladder<T>;
    using S = Sup<T, L>;
    beatOnce<S, L>();
    auto const lastBeat = FakeClock::now();
    int const  taken    = silentUntilPolicy<S>(100);
    CHECK(callsOf<T>() == 2 && calls[calls.size() - 2].n == 0 && calls.back().n == 1);
    // 60 ms to rung 1, 40 to rung 2 (30 ms window + one period), 40 to the policy
    CHECK(taken == 14);
    CHECK(static_cast<std::uint32_t>((FakeClock::now() - lastBeat).count())
          == S::worstToEscalationMs<L>() - S::timeoutMs);
    CHECK(Seen::all.back().name == "ladder" && Seen::all.back().silentMs == 40
          && Seen::all.back().allowedMs == 30);
    auto const recovery = H::lastRecovery.load();
    CHECK(recovery && recovery->escalatedAttempts == 2 && recovery->total == 0);
    auto const starvation = H::lastStarvation.load();
    CHECK(starvation && starvation->check == 0);
    int const fed = Watchdog<T>::feeds;
    turns<S>(10);
    CHECK(Watchdog<T>::feeds == fed && callsOf<T>() == 2);
    H::lastStarvation.clear();
}

void cannot() {
    using Kvasir::Test::test;
    test("Recovery::cannot on rung 1: the policy in the same turn, no recovery window");

    struct T {};

    using L   = Ladder<T>;
    using S   = Sup<T, L>;
    L::answer = H::Recovery::cannot;
    beatOnce<S, L>();
    int const taken = silentUntilPolicy<S>(100);
    CHECK(taken == 6 && callsOf<T>() == 1);
    auto const recovery = H::lastRecovery.load();
    CHECK(recovery && recovery->escalatedAttempts == 2);   // the ladder is spent
    H::lastStarvation.clear();
}

void signatures() {
    using Kvasir::Test::test;
    test(
      "the four forms: void(), Recovery(), void(Attempt), Recovery(Attempt); defaults 1 rung, "
      "window = maxInterval");

    struct T1 {};

    struct T2 {};

    struct T3 {};

    using S                  = Sup<T1, PlainVoid<T1>, PlainRecovery<T2>, VoidAttempt<T3>>;
    std::size_t const policy = Seen::all.size();
    beatOnce<S, PlainVoid<T1>, PlainRecovery<T2>, VoidAttempt<T3>>();
    turns<S>(7);   // 50 ms > 40: one per turn, in list order
    CHECK(callsOf<T1>() == 1 && callsOf<T2>() == 1 && callsOf<T3>() == 1);
    CHECK(calls.back().tag == tagId<T3>() && calls.back().n == 0 && calls.back().of == 1);
    turnsBeating<S, PlainVoid<T1>, PlainRecovery<T2>, VoidAttempt<T3>>(1);
    CHECK(S::recoveredCount(0) == 1 && S::recoveredCount(1) == 1 && S::recoveredCount(2) == 1);
    CHECK(Seen::all.size() == policy);
}

void budget() {
    using Kvasir::Test::test;
    test("an onStarved over the budget (5 ms > 2 ms): counted, the recovery goes on");

    struct T {};

    using L = Ladder<T>;
    using S = Sup<T, L>;
    L::slow = true;
    beatOnce<S, L>();
    turns<S>(6);
    CHECK(callsOf<T>() == 1 && S::recoveryOverruns() == 1);
    turnsBeating<S, L>(1);
    CHECK(S::recoveredCount(0) == 1);
}

void twoInOneTurn() {
    using Kvasir::Test::test;
    test(
      "two checks past their window in one turn: one onStarved per turn, the second the turn "
      "after");

    struct T {};

    using S = Sup<T, Ladder<T>, Other<T>>;
    beatOnce<S, Ladder<T>, Other<T>>();
    std::size_t const policy = Seen::all.size();
    turns<S>(6);
    CHECK(callsOf<T>() == 1 && callsOf<Other<T>>() == 0 && Seen::all.size() == policy);
    turns<S>(1);
    CHECK(callsOf<Other<T>>() == 1 && Seen::all.size() == policy);
}

void bareEscalatesWhileOtherRecovers() {
    using Kvasir::Test::test;
    test(
      "a check without onStarved escalates at once while another is in its recovery window; then "
      "no more rungs");

    struct T {};

    using S = Sup<T, Ladder<T>, Plain<T>>;
    beatOnce<S, Ladder<T>>();
    std::size_t const policy = Seen::all.size();
    // the ladder's rung 1 at turn 6 (window 30 ms, rung 2 would be turn 10); "bare" beats twice, then starves at
    // turn 8 - inside the ladder's recovery window
    turnsBeating<S, Plain<T>>(2);
    turns<S>(4);
    turns<S>(1);   // turn 7
    CHECK(callsOf<T>() == 1 && Seen::all.size() == policy);
    turns<S>(1);
    CHECK(Seen::all.size() == policy + 1 && Seen::all.back().name == "bare");
    int const fed = Watchdog<T>::feeds;
    turns<S>(10);
    CHECK(callsOf<T>() == 1 && Watchdog<T>::feeds == fed);   // latched: no rung, no feed
    H::lastStarvation.clear();
}

void suspendDuringRecovery() {
    using Kvasir::Test::test;
    test("suspended in its recovery window: no rung while suspended; resume starts a fresh ladder");

    struct T {};

    using L = Ladder<T>;
    using S = Sup<T, L>;
    beatOnce<S, L>();
    turns<S>(6);
    CHECK(callsOf<T>() == 1 && S::attemptOf(0) == 1);
    S::suspend<L>();
    turns<S>(20);
    CHECK(callsOf<T>() == 1);
    S::resume<L>();
    CHECK(S::attemptOf(0) == 0);
    turns<S>(5);   // the full 50 ms window again, not the 30 ms recovery window
    CHECK(callsOf<T>() == 1);
    turns<S>(1);
    CHECK(callsOf<T>() == 2 && calls.back().n == 0);
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

    struct T {};

    using S       = Sup<T, Ladder<T>>;
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

template<typename Tag, typename... Cs>
using DSup = H::Supervisor<Startup<Tag, Cs...>, Watchdog<Tag>, FakeClock, Deferred10>;

// the watchdog sees no onStarved: a budget past its timeout is allowed (500 ms here)
struct DeferredSlowBudget {
    static constexpr auto period          = 10ms;
    static constexpr bool deferRecoveries = true;
    static constexpr auto recoveryBudget  = 600ms;
};

static_assert(DSup<Ct,
                   Ladder<Ct>>::deferRecoveries
              && !Sup<Ct,
                      Ladder<Ct>>::deferRecoveries);
static_assert(H::Supervisor<Startup<Ct,
                                    Ladder<Ct>>,
                            Watchdog<Ct>,
                            FakeClock,
                            DeferredSlowBudget>::budgetBelowTimeout);
// the window and the rungs do not change: the worst case holds as it is
static_assert(DSup<Ct,
                   Ladder<Ct>>::worstToEscalationMs<Ladder<Ct>>()
              == 640);

// a ladder check whose onStarved runs a hook (a service() preempting it)
template<typename Tag>
struct Hooked {
    static constexpr std::string_view   name             = "hooked";
    static constexpr auto               maxInterval      = 50ms;
    static constexpr auto               graceAfterBoot   = 0ms;
    static constexpr auto               recoveryAttempts = 2;
    static constexpr auto               recoveryWindow   = 30ms;
    static inline std::function<void()> during{};
    static inline H::Recovery           answer = H::Recovery::started;

    static H::Recovery onStarved(H::Attempt a) {
        calls.push_back({tagId<Tag>(), a.n, a.of, inProgressNow()});
        if(during) { during(); }
        return answer;
    }
};

void deferredRequestRunRecover() {
    using Kvasir::Test::test;
    test("deferred: service() requests at the first period past the window, runRecoveries() calls");

    struct T {};

    using L = Ladder<T>;
    using S = DSup<T, L>;
    beatOnce<S, L>();
    std::size_t const policy = Seen::all.size();
    CHECK(silentUntilRequested<S>(10) == 6);   // 60 ms silent, as without deferRecoveries
    CHECK(callsOf<T>() == 0 && S::attemptOf(0) == 1 && inProgressNow() == 0);
    CHECK(S::runRecoveries());
    CHECK(callsOf<T>() == 1 && calls.back().n == 0 && calls.back().of == 2);
    CHECK(calls.back().inProgress == H::recoveryInProgress(0, 1));
    CHECK(inProgressNow() == 0 && S::pendingRecoveries() == 0);
    CHECK(!S::runRecoveries() && callsOf<T>() == 1);

    test("deferred: fed through the window, a beat recovers");
    int const fed = Watchdog<T>::feeds;
    turns<S>(1);
    CHECK(Watchdog<T>::feeds == fed + 1);
    turnsBeating<S, L>(1);
    CHECK(S::recoveredCount(0) == 1 && S::attemptOf(0) == 0 && Seen::all.size() == policy);
    CHECK(H::lastRecovery.load() && H::lastRecovery.load()->total == 1);
}

void deferredNeverRun() {
    using Kvasir::Test::test;
    test("deferred: a loop that never runs the requests escalates after the last rung's window");

    struct T {};

    using L = Ladder<T>;
    using S = DSup<T, L>;
    beatOnce<S, L>();
    std::size_t const before = Seen::all.size();
    int const         taken  = silentUntilPolicy<S>(30);
    CHECK(taken > 0 && taken * 10 + 10 <= static_cast<int>(S::worstToEscalationMs<L>()));
    CHECK(Seen::all.size() == before + 1 && Seen::all.back().check == 0);
    CHECK(callsOf<T>() == 0 && S::attemptOf(0) == 2);
    auto const r = H::lastRecovery.load();
    CHECK(r && r->escalatedAttempts == 2 && r->inProgress == 0);
}

void deferredCannot() {
    using Kvasir::Test::test;
    test("deferred: Recovery::cannot escalates at the next service()");

    struct T {};

    using L   = Ladder<T>;
    using S   = DSup<T, L>;
    L::answer = H::Recovery::cannot;
    beatOnce<S, L>();
    CHECK(silentUntilRequested<S>(10) == 6);
    std::size_t const before = Seen::all.size();
    CHECK(S::runRecoveries() && callsOf<T>() == 1);
    CHECK(Seen::all.size() == before);
    turns<S>(1);
    CHECK(Seen::all.size() == before + 1 && Seen::all.back().check == 0);
    // named with the silence that led to the request (60 ms of 50) and the 10 ms since, as without deferRecoveries
    CHECK(Seen::all.back().silentMs == 70 && Seen::all.back().allowedMs == 50);
    CHECK(callsOf<T>() == 1);
}

void deferredBeatCancels() {
    using Kvasir::Test::test;
    test("deferred: a beat before the loop got to the request cancels it");

    struct T {};

    using L = Ladder<T>;
    using S = DSup<T, L>;
    beatOnce<S, L>();
    CHECK(silentUntilRequested<S>(10) == 6);
    turnsBeating<S, L>(1);
    CHECK(S::pendingRecoveries() == 0 && !S::runRecoveries() && callsOf<T>() == 0);
    CHECK(S::attemptOf(0) == 0 && S::recoveredCount(0) == 1);
}

void deferredServiceInsideOnStarved() {
    using Kvasir::Test::test;
    test("deferred: service() preempting onStarved (the check beats): the record ends consistent");

    struct T {};

    using L = Hooked<T>;
    using S = DSup<T, L>;
    beatOnce<S, L>();
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
    turns<S>(6);
    CHECK(S::attemptOf(0) == 1);
    turns<S>(4);   // past the 30 ms window: rung 2 replaces rung 1
    CHECK(S::attemptOf(0) == 2 && S::pendingRecoveries() == 1);
    int const before = callsOf<T>();
    CHECK(S::runRecoveries() && !S::runRecoveries());
    CHECK(callsOf<T>() == before + 1 && calls.back().n == 1);
}

void deferredCannotAfterBeat() {
    using Kvasir::Test::test;
    test("deferred: Recovery::cannot from a check that beat during the call does not escalate it");

    struct T {};

    using L   = Hooked<T>;
    using S   = DSup<T, L>;
    L::answer = H::Recovery::cannot;
    beatOnce<S, L>();
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
    turns<S>(4);   // silent turns inside its 50 ms window: no stale refusal left
    CHECK(Seen::all.size() == before && callsOf<T>() == 1 && S::pendingRecoveries() == 0);
}

void deferredLatchDropsRequests() {
    using Kvasir::Test::test;
    test("deferred: a policy that stops feeding drops the requests the loop has not run");

    struct T {};

    using S = DSup<T, Ladder<T>, Plain<T>>;
    beatOnce<S, Ladder<T>>();
    std::size_t const policy = Seen::all.size();
    // as bareEscalatesWhileOtherRecovers: the ladder's request at turn 6, "bare" starves at turn 8
    turnsBeating<S, Plain<T>>(2);
    turns<S>(4);
    turns<S>(1);   // turn 7
    CHECK(S::pendingRecoveries() == 1U && Seen::all.size() == policy);
    turns<S>(1);
    CHECK(Seen::all.size() == policy + 1 && Seen::all.back().name == "bare");
    CHECK(S::pendingRecoveries() == 0 && !S::runRecoveries() && callsOf<T>() == 0);
    H::lastStarvation.clear();
}

void deferredWithoutOnStarved() {
    using Kvasir::Test::test;
    test(
      "deferred: an image without onStarved keeps runRecoveries() in its main loop, which does "
      "nothing");

    struct T {};

    using S = DSup<T, Plain<T>>;
    static_assert(S::deferRecoveries && !S::Cfg::anyRecovery);
    S::service();
    CHECK(!S::runRecoveries() && S::pendingRecoveries() == 0);
}

void deferredTurn() {
    using Kvasir::Test::test;
    test(
      "deferred: turn() runs one requested recovery per call, busy while more are pending, then "
      "idle");

    struct T {};

    using L = Ladder<T>;
    using M = Hooked<T>;   // a second ladder check, its own name
    using S = DSup<T, L, M>;
    beatOnce<S, L, M>();
    CHECK(S::turn().isIdle());   // nothing requested: nothing to wait for
    CHECK(silentUntilRequested<S>(10) == 6);
    // both silent: one onStarved request per turn, the second check's a turn later
    CHECK(turnsUntil<S>(3, [] { return S::pendingRecoveries() == 0b11; }) != -1);
    auto const first = S::turn();
    CHECK(callsOf<T>() == 1 && first.isBusy());
    auto const second = S::turn();
    CHECK(callsOf<T>() == 2 && second.isIdle());
    CHECK(S::pendingRecoveries() == 0 && S::turn().isIdle() && callsOf<T>() == 2);
}
}   // namespace

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::HealthFixture::TestPolicy;
};

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
    deferredTurn();
    return Kvasir::Test::report();
}
