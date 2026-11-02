// Kvasir::Test (src/kvasir/Test): the macros, the list, the run state's decisions, PerfState, and OnRequest's turn on
// the host - with a capturing UC_LOG_* (defined before the headers: Test.hpp then includes no uc_log) and a fake
// clock (no real time is read).
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace capture {
inline std::vector<std::string> lines;

template<typename T>
std::string text(T const& v) {
    if constexpr(std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr(std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr(std::is_integral_v<T>) {
        return std::to_string(v);
    } else if constexpr(requires { v.count(); }) {
        return std::to_string(v.count());
    } else {
        return "?";
    }
}

// "{...}" replaced by the arguments in order, "{{" / "}}" by one brace
template<typename Fmt,
         typename... Args>
void record(char       level,
            Fmt const& fmt,
            Args const&... args) {
    std::string_view const         f{fmt};
    std::vector<std::string> const a{text(args)...};
    std::string                    out{level, ' '};
    std::size_t                    next = 0;
    for(std::size_t i = 0; i < f.size(); ++i) {
        if(f[i] == '{' && i + 1 < f.size() && f[i + 1] == '{') {
            out += '{';
            ++i;
        } else if(f[i] == '}' && i + 1 < f.size() && f[i + 1] == '}') {
            out += '}';
            ++i;
        } else if(f[i] == '{') {
            while(i < f.size() && f[i] != '}') { ++i; }
            out += next < a.size() ? a[next] : "<missing>";
            ++next;
        } else {
            out += f[i];
        }
    }
    lines.push_back(out);
}

inline bool has(std::string_view part) {
    for(auto const& l : lines) {
        if(l.contains(part)) { return true; }
    }
    return false;
}

inline std::size_t count(std::string_view part) {
    std::size_t n = 0;
    for(auto const& l : lines) {
        if(l.contains(part)) { ++n; }
    }
    return n;
}
}   // namespace capture

#define UC_LOG_T(...) ::capture::record('T', __VA_ARGS__)
#define UC_LOG_D(...) ::capture::record('D', __VA_ARGS__)
#define UC_LOG_I(...) ::capture::record('I', __VA_ARGS__)
#define UC_LOG_W(...) ::capture::record('W', __VA_ARGS__)
#define UC_LOG_E(...) ::capture::record('E', __VA_ARGS__)
#define UC_LOG_C(...) ::capture::record('C', __VA_ARGS__)

#include "kvasir/Test/OnRequest.hpp"
#include "kvasir/Test/Test.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <source_location>

using Kvasir::Test::Context;
using Kvasir::Test::Evidence;
using Kvasir::Test::Outcome;
using Kvasir::Test::Reason;
using Kvasir::Test::Request;
using Kvasir::Test::RunState;
using Kvasir::Test::TestInfo;
using Kvasir::Test::Verdict;
namespace D = Kvasir::Test::Detail;

namespace {
int afterExpect = 0;   // set by a body after its failed EXPECT, never after a failed REQUIRE
int afterSkip   = 0;
}   // namespace

KVASIR_TEST(Macros,
            ExpectGoesOn) {
    KVASIR_EXPECT_EQ(1, 2);
    afterExpect = 1;
}

KVASIR_TEST(Macros,
            RequireEnds) {
    KVASIR_REQUIRE_EQ(1, 2);
    afterExpect = 2;
}

KVASIR_TEST(Macros,
            SkipEnds) {
    KVASIR_SKIP("no part {here}");
    afterSkip = 1;
}

KVASIR_TEST(Macros,
            AllHold) {
    KVASIR_EXPECT(1 + 1 == 2);
    KVASIR_EXPECT_NE(1, 2);
    KVASIR_EXPECT_LT(-1, 0U);   // by value, not by conversion: -1 < 0u
    KVASIR_EXPECT_LE(2, 2);
    KVASIR_EXPECT_GT(3, 2);
    KVASIR_EXPECT_GE(3, 3);
    KVASIR_EXPECT_NEAR(10U, 12U, 2U);   // unsigned, a below b: no wrap
    KVASIR_EXPECT_NEAR(std::chrono::milliseconds{5},
                       std::chrono::milliseconds{4},
                       std::chrono::milliseconds{1});
}

KVASIR_TEST(Macros,
            NearFails) {
    KVASIR_EXPECT_NEAR(10, 13, 2);
}

KVASIR_TEST_WITH(Cross,
                 Panics,
                 {.outcome = Outcome::panic,
                  .cause   = Kvasir::Panic::Cause::checkFailed}) {}

KVASIR_TEST_WITH(Cross,
                 Off,
                 {.enabled = false}) {}

KVASIR_TEST(Unlisted,
            Forgotten) {}   // defined, not listed: the host tool's job (kvasir_test.py list)

KVASIR_TEST_SUITE(Macros,
                  ExpectGoesOn,
                  RequireEnds,
                  SkipEnds);

using Listed
  = Kvasir::Test::List<KvasirTests::Macros::Suite,
                       KvasirTests::Macros::AllHold,
                       Kvasir::Test::List<KvasirTests::Cross::Panics, KvasirTests::Cross::Off>>;
using Flat = D::Flatten<Listed>::type;

static_assert(std::is_same_v<Flat,
                             Kvasir::Test::List<KvasirTests::Macros::ExpectGoesOn,
                                                KvasirTests::Macros::RequireEnds,
                                                KvasirTests::Macros::SkipEnds,
                                                KvasirTests::Macros::AllHold,
                                                KvasirTests::Cross::Panics,
                                                KvasirTests::Cross::Off>>,
              "a List flattens in order: a test's index is its position");
static_assert(D::Table<Flat>::count == 6);
static_assert(D::Table<Flat>::infos[4].outcome == Outcome::panic);
static_assert(!D::Table<Flat>::infos[5].enabled);
static_assert(std::string_view{KvasirTests::Macros::AllHold::suite} == "Macros");
static_assert(std::string_view{KvasirTests::Macros::AllHold::name} == "AllHold");

// a fake clock: every now() is 10 us after the last, no real time
struct FakeClock {
    using rep                       = std::int64_t;
    using period                    = std::micro;
    using duration                  = std::chrono::duration<rep, period>;
    using time_point                = std::chrono::time_point<FakeClock>;
    static constexpr bool is_steady = true;
    static inline rep     us        = 0;

    static time_point now() {
        us += 10;
        return time_point{duration{us}};
    }
};

struct Config : Kvasir::Test::DefaultConfig {
    using Clock = FakeClock;
};

namespace {
// an optional's value, or the test ends here
template<typename T>
T const& must(std::optional<T> const& o,
              std::source_location    loc = std::source_location::current()) {
    if(!o.has_value()) {
        Kvasir::Test::fail("an optional without a value", loc);
        std::abort();
    }
    return *o;
}

template<typename T>
D::Result runSlot() {
    Kvasir::Test::PerfState none{0, &D::noClock};
    return D::Slot<0, T>::run(none);
}

void macros() {
    Kvasir::Test::test("macros");
    capture::lines.clear();
    auto r = runSlot<KvasirTests::Macros::ExpectGoesOn>();
    CHECK_EQ(r.failures, 1U);
    CHECK_EQ(afterExpect, 1);   // the body went on
    CHECK(capture::has("E TEST|EXPECT|0|1 == 2 failed: 1 vs 2"));

    r = runSlot<KvasirTests::Macros::RequireEnds>();
    CHECK_EQ(r.failures, 1U);
    CHECK_EQ(afterExpect, 1);   // not 2: the REQUIRE returned

    capture::lines.clear();
    r = runSlot<KvasirTests::Macros::SkipEnds>();
    CHECK(r.skipped);
    CHECK_EQ(afterSkip, 0);
    CHECK(capture::has(
      "W TEST|SKIPPED|0|no part {here}"));   // braces escaped in the format, shown as written

    capture::lines.clear();
    r = runSlot<KvasirTests::Macros::AllHold>();
    CHECK_EQ(r.failures, 0U);
    CHECK(capture::lines.empty());

    r = runSlot<KvasirTests::Macros::NearFails>();
    CHECK_EQ(r.failures, 1U);
    CHECK(capture::has("10 ~= 13 +- 2 failed: 10 vs 13"));

    Context c{7};
    CHECK(c.verdict() == Verdict::pass);
    c.fail();
    CHECK(c.verdict() == Verdict::fail);
    c.skip();
    CHECK(c.verdict() == Verdict::skip);
}

constexpr std::array<TestInfo, 4> infos{
  {{Outcome::returns, {}, true},
   {Outcome::panic, Kvasir::Panic::Cause::assertion, true},
   {Outcome::watchdog, {}, true},
   {Outcome::fault, {}, true}}
};

Request requestFor(std::uint32_t seq,
                   std::uint32_t select,
                   std::uint32_t flags = 0) {
    Request r{};
    r.magic     = Request::magicValue;
    r.seq       = seq;
    r.flags     = flags;
    r.select[0] = select;
    return r;
}

RunState runningWith(unsigned inFlight) {
    auto s     = D::freshRun(4, 0, nullptr);
    s.inFlight = static_cast<std::uint16_t>(inFlight);
    s.next     = static_cast<std::uint16_t>(inFlight);
    return s;
}

void atBoot() {
    Kvasir::Test::test("atBoot");
    Request const none{};   // power-up garbage reads as no magic
    // power-up: nothing stored, no request
    auto d = D::atBoot(std::nullopt, none, Evidence{}, infos);
    CHECK(d.start == D::Start::noRun);
    CHECK_EQ(d.state.count, 4U);

    // a request without a valid run state (power-up, another image ran in between): a leftover, not taken - READY
    auto const r = requestFor(5, 0b1010U, Request::stopAtFirstFailure);
    d            = D::atBoot(std::nullopt, r, Evidence{}, infos);
    CHECK(d.start == D::Start::noRun);

    // a new request beside a valid state: a fresh run of what it selects, from the request
    auto idle  = D::freshRun(4, 0, nullptr);
    idle.phase = static_cast<std::uint8_t>(Kvasir::Test::Phase::done);
    d          = D::atBoot(idle, r, Evidence{}, infos);
    CHECK(d.start == D::Start::fresh);
    CHECK_EQ(d.state.requestSeq, 5U);
    CHECK(!D::isSelected(d.state, 0) && D::isSelected(d.state, 1) && D::isSelected(d.state, 3));
    CHECK_EQ(D::nextSelected(d.state, 0), 1U);
    CHECK((d.state.flags & RunState::stopAtFirstFail) != 0);
    CHECK_EQ(D::servedSeq(d.state), 5U);

    // the same request again (a reset after it was taken): not new
    auto taken = d.state;
    d          = D::atBoot(taken, r, Evidence{}, infos);
    CHECK(d.start == D::Start::resumed);
    CHECK_EQ(d.state.boots, 2U);

    // a new request mid-run wins, its in-flight test dropped
    taken.inFlight = 2;
    d              = D::atBoot(taken, requestFor(6, 1U), Evidence{.panic = true}, infos);
    CHECK(d.start == D::Start::fresh);
    CHECK(!d.judged.has_value());
    CHECK_EQ(d.state.requestSeq, 6U);

    // the expected panic: PASS
    d = D::atBoot(runningWith(2),
                  none,
                  Evidence{.panic = true, .cause = Kvasir::Panic::Cause::assertion},
                  infos);
    CHECK(d.start == D::Start::resumed && d.judged && must(d.judged).verdict == Verdict::pass);
    CHECK_EQ(d.judgedIndex, 1U);
    CHECK(D::verdictOf(d.state, 1) == Verdict::pass);
    CHECK_EQ(d.state.pass, 1U);
    CHECK_EQ(d.state.next, 2U);
    CHECK_EQ(d.state.inFlight, 0U);

    // a panic with another cause: FAIL wrongCause
    d = D::atBoot(runningWith(2),
                  none,
                  Evidence{.panic = true, .cause = Kvasir::Panic::Cause::abort},
                  infos);
    CHECK(must(d.judged).verdict == Verdict::fail && must(d.judged).reason == Reason::wrongCause);

    // a returns test in flight after a panic: FAIL crashed; after the watchdog: FAIL timeout; else FAIL reset
    d = D::atBoot(runningWith(1), none, Evidence{.panic = true}, infos);
    CHECK(must(d.judged).reason == Reason::crashedPanic);
    d = D::atBoot(runningWith(1),
                  none,
                  Evidence{.panic = true, .cause = Kvasir::Panic::Cause::fault},
                  infos);
    CHECK(must(d.judged).reason == Reason::crashedFault);
    d = D::atBoot(runningWith(1), none, Evidence{.watchdog = true}, infos);
    CHECK(must(d.judged).reason == Reason::timeout);
    d = D::atBoot(runningWith(1), none, Evidence{}, infos);
    CHECK(must(d.judged).reason == Reason::reset);
    CHECK_EQ(d.state.fail, 1U);

    // the watchdog test: the watchdog reset alone passes, a record with it does not
    d = D::atBoot(runningWith(3), none, Evidence{.watchdog = true}, infos);
    CHECK(must(d.judged).verdict == Verdict::pass);
    d = D::atBoot(runningWith(3), none, Evidence{.panic = true, .watchdog = true}, infos);
    CHECK(must(d.judged).reason == Reason::notWatchdog);

    // the fault test: a fault record, or a panic "fault"
    d = D::atBoot(runningWith(4), none, Evidence{.fault = true}, infos);
    CHECK(must(d.judged).verdict == Verdict::pass);
    d = D::atBoot(runningWith(4),
                  none,
                  Evidence{.panic = true, .cause = Kvasir::Panic::Cause::fault},
                  infos);
    CHECK(must(d.judged).verdict == Verdict::pass);
    d = D::atBoot(runningWith(4), none, Evidence{.watchdog = true}, infos);
    CHECK(must(d.judged).reason == Reason::noFault);

    // a run that is done: no run (READY), its last seq kept so the same request is not taken twice
    auto done       = runningWith(0);
    done.phase      = static_cast<std::uint8_t>(Kvasir::Test::Phase::done);
    done.requestSeq = 9;
    d               = D::atBoot(done, requestFor(9, 1U), Evidence{}, infos);
    CHECK(d.start == D::Start::noRun);
    CHECK_EQ(d.state.requestSeq, 9U);
    // a run of everything without a request keeps the last seq, and its DONE says seq=0
    auto const all = D::freshRun(4, 9, nullptr);
    CHECK_EQ(all.requestSeq, 9U);
    CHECK_EQ(D::servedSeq(all), 0U);
    CHECK(!D::requestIsNew(requestFor(9, 1U), all));

    // a state of another image (another count): not resumed
    auto other  = runningWith(1);
    other.count = 3;
    d           = D::atBoot(other, none, Evidence{}, infos);
    CHECK(d.start == D::Start::noRun);

    // stop at the first failure
    auto stop = D::freshRun(4, 0, nullptr);
    CHECK(!D::stopsAfter(stop, Verdict::fail));
    stop.flags |= RunState::stopAtFirstFail;
    CHECK(D::stopsAfter(stop, Verdict::fail) && !D::stopsAfter(stop, Verdict::pass));

    // two verdict bits a test: neighbours untouched
    RunState v{};
    D::setVerdict(v, 15, Verdict::skip);
    D::setVerdict(v, 16, Verdict::fail);
    D::setVerdict(v, 15, Verdict::pass);
    CHECK(D::verdictOf(v, 15) == Verdict::pass && D::verdictOf(v, 16) == Verdict::fail);
    CHECK(D::verdictOf(v, 14) == Verdict::none);
}

void perf() {
    Kvasir::Test::test("perf");
    // a source that wraps: samples 5, 7, 6 after a warm-up of 1000 (not counted)
    static constexpr std::array<std::uint32_t, 5> ticks{0xFFFF'FFF0U,
                                                        0xFFFF'FFF0U + 1000U,
                                                        0xFFFF'FFF0U + 1005U,
                                                        0xFFFF'FFF0U + 1012U,
                                                        0xFFFF'FFF0U + 1018U};
    static std::size_t                            at = 0;
    Kvasir::Test::PerfState                       p{3, [] { return ticks[at++]; }};
    unsigned                                      runs = 0;
    while(p.keepRunning()) { ++runs; }
    CHECK_EQ(runs, 4U);   // the warm-up and three counted iterations
    CHECK_EQ(p.samples(), 3U);
    CHECK_EQ(p.min(), 5U);
    CHECK_EQ(p.max(), 7U);
    CHECK_EQ(p.mean(), 6U);
}

// OnRequest in an "application": two tests that return, one of them failing
int selfTestRuns = 0;
}   // namespace

KVASIR_TEST(Self,
            Ok) {
    ++selfTestRuns;
}

KVASIR_TEST(Self,
            Bad) {
    ++selfTestRuns;
    KVASIR_EXPECT_GT(1, 2);
}

KVASIR_TEST(Self,
            Third) {
    ++selfTestRuns;
}

namespace {
using SelfTest = Kvasir::Test::OnRequest<
  Config,
  Kvasir::Test::List<KvasirTests::Self::Ok, KvasirTests::Self::Bad, KvasirTests::Self::Third>>;

void write(Request const& r) {
    kvasir_test_request.flags = r.flags;
    for(std::size_t i = 0; i != std::size(r.select); ++i) {
        kvasir_test_request.select[i] = r.select[i];
    }
    kvasir_test_request.seq   = r.seq;
    kvasir_test_request.magic = r.magic;
}

void onRequest() {
    Kvasir::Test::test("onRequest");
    kvasir_test_run.clear();
    kvasir_test_request.magic = 0;
    SelfTest::runtimeInit();   // nothing stored: nothing to say
    capture::lines.clear();
    CHECK(SelfTest::turn() == Kvasir::Turn::idle());
    CHECK_EQ(selfTestRuns, 0);   // no request

    write(requestFor(3, 0b111U));
    (void)SelfTest::turn();
    CHECK_EQ(selfTestRuns, 3);
    auto s = kvasir_test_run.load();
    CHECK(s.has_value());
    CHECK_EQ(must(s).pass, 2U);
    CHECK_EQ(must(s).fail, 1U);
    CHECK(must(s).phase == static_cast<std::uint8_t>(Kvasir::Test::Phase::done));
    CHECK(D::verdictOf(must(s), 1) == Verdict::fail);
    CHECK(capture::has("I TEST|START|seq=3|n=3|of=3"));
    CHECK(capture::has("I TEST|PASS|0|Self.Ok|"));
    CHECK(capture::has("E TEST|EXPECT|1|1 > 2 failed: 1 vs 2"));
    CHECK(capture::has("E TEST|FAIL|1|Self.Bad|1 failed|"));
    CHECK(capture::has("I TEST|DONE|pass=2|fail=1|skip=0|boots=1|seq=3"));

    (void)SelfTest::turn();   // the same request again: nothing
    CHECK_EQ(selfTestRuns, 3);

    // a new one that selects test 1 and 2 and stops at the first failure: test 2 does not run
    write(requestFor(4, 0b110U, Request::stopAtFirstFailure));
    (void)SelfTest::turn();
    CHECK_EQ(selfTestRuns, 4);
    s = kvasir_test_run.load();
    CHECK(D::verdictOf(must(s), 0) == Verdict::none && D::verdictOf(must(s), 2) == Verdict::none);
    CHECK_EQ(must(s).fail, 1U);

    // a reset in the middle of a test: the next boot fails it and ends the run
    auto cut     = D::freshRun(3, 0, nullptr);
    cut.inFlight = 2;
    cut.next     = 2;
    kvasir_test_run.store(cut);
    capture::lines.clear();
    SelfTest::runtimeInit();
    s = kvasir_test_run.load();
    CHECK(D::verdictOf(must(s), 1) == Verdict::fail);
    CHECK(must(s).phase == static_cast<std::uint8_t>(Kvasir::Test::Phase::done));
    CHECK_EQ(must(s).boots, 2U);
    CHECK(capture::has("TEST|AFTER|1|Self.Bad|"));
    CHECK_EQ(capture::count("TEST|DONE|"), 1U);
}

void layout() {
    Kvasir::Test::test("layout");
    // what kvasir_test.py reads: the record is the id, 29 words of state, the CRC
    static_assert(sizeof(kvasir_test_run) == 31 * 4);
    static_assert(offsetof(RunState, verdicts) == 20 && offsetof(RunState, selected) == 84);
    static_assert(offsetof(Request, select) == 12);
    CHECK_EQ(Request::magicValue, 0x5152'544BU);        // "KTRQ" in memory
    CHECK_EQ(Kvasir::Test::RunTag::id, 0x5453'544BU);   // "KTST"
}
}   // namespace

int main() {
    macros();
    atBoot();
    perf();
    onRequest();
    layout();
    return Kvasir::Test::report();
}
