// Tests for Kvasir::RateLimiter / Kvasir::CountLimiter: storm-scoped backoff, one
// detail line per new key, the burst budget, conserved dropped counts, storm
// boundaries and the occurrence based variant.
#include "kvasir/Util/RateLimiter.hpp"
#include "kvasir_test.hpp"

#include <chrono>
#include <print>
#include <vector>

using namespace Kvasir;
using namespace Kvasir::Test;
using namespace std::chrono_literals;

#include "support/FakeClock.hpp"

using tp = FakeClock::time_point;
using ms = std::chrono::milliseconds;

static tp at(ms t) { return tp{} + t; }

static constexpr std::uint32_t K(std::uint32_t i) { return i; }

// Runs `keys` round-robin every `step` from 0 until `end` (exclusive), returns the
// times of the allowed reports and their suppressed counts.
struct Run {
    std::vector<ms>            reported;
    std::vector<std::uint32_t> suppressed;
    std::uint32_t              occurrences{};
};

template<typename L>
static Run storm(L&                                l,
                 std::vector<std::uint32_t> const& keys,
                 ms                                step,
                 ms                                end) {
    Run         r;
    std::size_t i = 0;
    for(ms t{0}; t < end; t += step, ++i) {
        ++r.occurrences;
        if(auto const d = l.allow(keys[i % keys.size()], at(t))) {
            r.reported.push_back(t);
            r.suppressed.push_back(d.suppressed);
        }
    }
    return r;
}

static void firstOccurrenceAtBoot() {
    test("firstOccurrenceAtBoot");
    RateLimiter<FakeClock> l;

    auto const d = l.allow(K(1), at(100ms));
    CHECK(d.allowed);
    CHECK_EQ(d.suppressed, 0U);
}

static void continuousStormBacksOff() {
    test("continuousStormBacksOff");
    RateLimiter<FakeClock> l;

    auto const r = storm(l, {K(1)}, 100ms, 60'000ms);
    CHECK(r.reported == std::vector<ms>{0ms, 1000ms, 3000ms, 7000ms, 15000ms, 31000ms});
    CHECK(r.suppressed == std::vector<std::uint32_t>{0, 9, 19, 39, 79, 159});
    CHECK(!l.allow(K(1), at(62'900ms)));
    CHECK(l.allow(K(1), at(63'000ms)));   // window capped at 32 s
}

static void suppressedCountReturnedWithNextReport() {
    test("suppressedCountReturnedWithNextReport");
    RateLimiter<FakeClock> l;

    CHECK_EQ(l.allow(K(1), at(0ms)).suppressed, 0U);
    for(int i = 0; i < 10; ++i) { (void)l.allow(K(1), at(ms{1 + i})); }
    CHECK_EQ(l.suppressed(), 10U);

    auto const d = l.allow(K(1), at(1000ms));
    CHECK(d.allowed);
    CHECK_EQ(d.suppressed, 10U);
    CHECK_EQ(l.suppressed(), 0U);
}

// The measured failure: one absent device reported under alternating keys must not
// restart the backoff on every key switch.
static void alternatingKeysShareBackoff() {
    test("alternatingKeysShareBackoff");
    RateLimiter<FakeClock> l;

    auto const r = storm(l, {K(1), K(2), K(3)}, 100ms, 60'000ms);
    CHECK(r.reported
          == std::vector<ms>{0ms, 100ms, 200ms, 1000ms, 3000ms, 7000ms, 15000ms, 31000ms});
}

static void newKeyMidStormOnce() {
    test("newKeyMidStormOnce");
    RateLimiter<FakeClock> l;

    std::vector<ms> reported;
    for(ms t{0}; t < 20'000ms; t += 100ms) {
        if(l.allow(K(1), at(t))) { reported.push_back(t); }
        if(t == 5000ms || t == 5100ms || t == 9000ms) {
            if(l.allow(K(2), at(t + 50ms))) { reported.push_back(t + 50ms); }
        }
    }
    // K(2): one detail line at 5050; the anchor schedule (7000, 15000) is untouched
    CHECK(reported == std::vector<ms>{0ms, 1000ms, 3000ms, 5050ms, 7000ms, 15000ms});
}

static void burstBudgetCapsDetails() {
    test("burstBudgetCapsDetails");
    RateLimiter<FakeClock> l;

    int allowed = 0;
    for(std::uint32_t i = 1; i <= 6; ++i) {
        if(l.allow(K(i), at(ms{i * 10}))) { ++allowed; }
    }
    CHECK_EQ(allowed, 4);
    CHECK_EQ(l.suppressed(), 2U);
    CHECK(l.allow(K(5), at(1000ms)));   // budget refilled (and the anchor is due)
}

static void detailsAreExactUpToSeenKeys() {
    test("detailsAreExactUpToSeenKeys");
    RateLimiter<FakeClock, RateLimiterConfig{.burst = 8}> l;

    // the keys a driver really produces: kind x address x cause, and a raw 0
    enum class Fault : std::uint8_t { abortSend = 1, abortRecv, timeout, sdaStuck };
    std::uint32_t const ks[] = {rateLimitKey(Fault::abortSend, 0x4b, 1U),
                                rateLimitKey(Fault::abortRecv, 0x4b, 1U),
                                rateLimitKey(Fault::timeout, 0x4b, 0U),
                                0U};
    for(std::uint32_t i = 0; i < 4; ++i) { CHECK(l.allow(ks[i], at(ms{10 * i}))); }
    for(std::uint32_t i = 0; i < 4; ++i) { CHECK(!l.allow(ks[i], at(ms{100 + 10 * i}))); }

    // a fifth kind evicts the oldest (round-robin), which is then shown once more;
    // the keys still remembered stay suppressed
    CHECK(l.allow(rateLimitKey(Fault::sdaStuck, 0x4b, 0U), at(200ms)));
    CHECK(l.allow(ks[0], at(210ms)));
    CHECK(!l.allow(ks[2], at(220ms)));
    CHECK(!l.allow(ks[3], at(230ms)));
}

static void quietGapResetsBackoff() {
    test("quietGapResetsBackoff");
    RateLimiter<FakeClock> l;

    (void)storm(l, {K(1)}, 100ms, 8000ms);   // anchors 0, 1, 3, 7 -> backoff 3
    CHECK(l.allow(K(1), at(72'000ms)));      // > 64 s quiet: a new storm
    CHECK(!l.allow(K(1), at(72'900ms)));
    CHECK(l.allow(K(1), at(73'000ms)));   // window is 1 s again
}

static void shortGapKeepsBackoff() {
    test("shortGapKeepsBackoff");
    RateLimiter<FakeClock> l;

    (void)storm(l, {K(1)}, 100ms, 8000ms);   // backoff 3, last anchor at 7000
    CHECK(l.allow(K(1), at(30'000ms)));      // due (>= 8 s since 7000) -> backoff 4
    CHECK(!l.allow(K(1), at(31'000ms)));
    CHECK(!l.allow(K(1), at(45'900ms)));
    CHECK(l.allow(K(1), at(46'000ms)));   // 16 s window
}

static void takeSummarySilentDuringStorm() {
    test("takeSummarySilentDuringStorm");
    RateLimiter<FakeClock> l;

    (void)storm(l, {K(1), K(2)}, 100ms, 10'000ms + 100ms);   // last occurrence at 10000
    CHECK_EQ(l.takeSummary(at(10'500ms)), 0U);
    CHECK_EQ(l.takeSummary(at(17'900ms)), 0U);   // repeat window is 8 s now
    CHECK(l.takeSummary(at(18'000ms)) > 0U);
    CHECK_EQ(l.takeSummary(at(18'001ms)), 0U);
}

static void countsConserved() {
    test("countsConserved");
    RateLimiter<FakeClock> l;

    auto const    r         = storm(l, {K(1), K(2), K(3)}, 100ms, 60'000ms);
    std::uint32_t accounted = static_cast<std::uint32_t>(r.reported.size());
    for(auto s : r.suppressed) { accounted += s; }
    accounted += l.takeSummary(at(200'000ms));
    CHECK_EQ(accounted, r.occurrences);
}

static void resetIsFresh() {
    test("resetIsFresh");
    RateLimiter<FakeClock> l;

    (void)storm(l, {K(1)}, 100ms, 8000ms);
    l.reset();
    CHECK(l.allow(K(1), at(8000ms)));
    CHECK(!l.allow(K(1), at(8900ms)));
    CHECK(l.allow(K(1), at(9000ms)));
}

static void customConfig() {
    test("customConfig");
    RateLimiter<FakeClock, RateLimiterConfig{.windowMs = 100, .burst = 2, .maxBackoff = 1}> l;

    CHECK(l.allow(K(1), at(0ms)));
    CHECK(l.allow(K(2), at(1ms)));     // detail
    CHECK(!l.allow(K(3), at(2ms)));    // burst 2
    CHECK(l.allow(K(1), at(100ms)));   // anchor: window 100 ms -> backoff 1 (200 ms)
    CHECK(!l.allow(K(1), at(299ms)));
    CHECK(l.allow(K(1), at(300ms)));   // capped at 200 ms, not 400
    CHECK(!l.allow(K(1), at(499ms)));
    CHECK(l.allow(K(1), at(500ms)));
}

static void allowWithoutTimeUsesClock() {
    test("allowWithoutTimeUsesClock");
    RateLimiter<FakeClock> l;

    FakeClock::current = at(5000ms);
    CHECK(l.allow(K(1)));
    CHECK(!l.allow(K(1)));
    FakeClock::current = at(6000ms);
    CHECK(l.allow(K(1)));
}

static void keyMixing() {
    test("keyMixing");
    enum class Kind : std::uint8_t { a, b };

    CHECK(rateLimitKey(Kind::a) != rateLimitKey(Kind::b));
    CHECK(rateLimitKey(Kind::a, 1) != rateLimitKey(Kind::a, 2));
    CHECK(rateLimitKey(Kind::a, 1) != rateLimitKey(1, Kind::a));
    CHECK(rateLimitKey(Kind::a, 0x50, 0x1U) == rateLimitKey(Kind::a, 0x50, 0x1U));
    CHECK(rateLimitKey(true) != rateLimitKey(false));
    static_assert(rateLimitKey(1, 2, 3) != 0);
}

static void countLimiterDoublingGaps() {
    test("countLimiterDoublingGaps");
    CountLimiter<> l;

    // reported occurrences: 1, 3, 6, 11, 20, ...  (gaps 1, 2, 4, 8)
    CHECK_EQ(l.allow().suppressed, 0U);
    CHECK(!l.allow());
    auto d = l.allow();
    CHECK(d.allowed && d.suppressed == 1);
    CHECK(!l.allow());
    CHECK(!l.allow());
    d = l.allow();
    CHECK(d.allowed && d.suppressed == 2);
    for(int i = 0; i < 4; ++i) { CHECK(!l.allow()); }
    d = l.allow();
    CHECK(d.allowed && d.suppressed == 4);
}

static void countLimiterGapCaps() {
    test("countLimiterGapCaps");
    CountLimiter<CountLimiterConfig{.maxGap = 4}> l;

    int reported = 0;
    for(int i = 0; i < 1 + 1 + 2 + 4 + 4 + 4 + 1; ++i) {
        if(l.allow()) { ++reported; }
    }
    CHECK_EQ(reported, 5);   // 1st, +1, +2, +4, +4 ; the trailing +1 is still pending
    CHECK_EQ(l.suppressed(), 1U);
}

int main() {
    firstOccurrenceAtBoot();
    continuousStormBacksOff();
    suppressedCountReturnedWithNextReport();
    alternatingKeysShareBackoff();
    newKeyMidStormOnce();
    burstBudgetCapsDetails();
    detailsAreExactUpToSeenKeys();
    quietGapResetsBackoff();
    shortGapKeepsBackoff();
    takeSummarySilentDuringStorm();
    countsConserved();
    resetIsFresh();
    customConfig();
    allowWithoutTimeUsesClock();
    keyMixing();
    countLimiterDoublingGaps();
    countLimiterGapCaps();

    if(failures == 0) {
        std::print("all rate limiter tests passed\n");
    } else {
        std::print("{} failure(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
