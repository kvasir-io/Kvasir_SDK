#pragma once
// On-device tests: a test is a type, a suite a list of them, both listed by hand like a Startup list.
//
//     #include <kvasir/Test/Test.hpp>
//
//     KVASIR_TEST(Rollover, Wraps) {
//         KVASIR_REQUIRE_EQ(next(0xFF'FFFFU), 0U);    // fatal: ends this test on failure
//         KVASIR_EXPECT_LT(distance(), 2);             // non-fatal: the test goes on
//     }
//     KVASIR_TEST_WITH(Panic, Assertion, {.outcome = Kvasir::Test::Outcome::panic,
//                                         .cause   = Kvasir::Panic::Cause::assertion}) {
//         assert(Kvasir::Test::opaque(0) != 0);      // the run reboots; the next boot judges the record
//     }
//     KVASIR_TEST_SUITE(Rollover, Wraps);              // KvasirTests::Rollover::Suite
//
//     using Tests = Kvasir::Test::List<KvasirTests::Rollover::Suite, KvasirTests::Panic::Assertion>;
//
// A test image runs them with Kvasir::Test::Runner<Config, Tests>::run() (Test/Runner.hpp); an application can carry
// some of them as a factory self-test, run on a host's request, with Kvasir::Test::OnRequest<Config, Tests> in its
// Startup list (Test/OnRequest.hpp). Perf tests: Test/Perf.hpp. The host side is Kvasir_SDK/tools/kvasir_test.py,
// which reads the list from the image's symbols and catches a test that is defined but not listed.
//
// The macros (production checks are KVASIR_CHECK*, Runtime/Check.hpp; test macros are never ASSERT/CHECK):
//   KVASIR_EXPECT(cond), KVASIR_EXPECT_EQ/NE/LT/LE/GT/GE(a, b), KVASIR_EXPECT_NEAR(a, b, eps)   - non-fatal
//   KVASIR_REQUIRE(cond), KVASIR_REQUIRE_EQ/NE/LT/LE/GT/GE(a, b), KVASIR_REQUIRE_NEAR(a, b, eps) - end the test
//   KVASIR_SKIP("reason")                                                                    - skip it, ending it
// A failed one logs "TEST|EXPECT|<index>|<expression> failed: <a> vs <b>" (error) - the operands need a log formatter
// (remote_fmt), else use the one-operand form. Integers compare by value (std::cmp_*, Runtime/Check.hpp's
// comparators), each operand is evaluated once. They work only inside a test body (they name its context,
// kvasirTestContext) and `return` from it, so a helper that checks takes the context and is called with a REQUIRE's
// effect spelled out (`if(!helper(kvasirTestContext)) { return; }`).
//
// Options (KVASIR_TEST_WITH's third argument, designated initialisers):
//   enabled   false: not run, reported SKIP; the body is still compiled (and kept in the image for the host's list),
//             so a body that names a block a chip lacks keeps that part under `if constexpr`
//   outcome   returns (the default), panic (cause: which Panic::Cause), fault, watchdog (Config::Watchdog), reset:
//             a test that must end the run; the next boot judges it from the panic/fault records and the reset kind
//   cause     for Outcome::panic
//   iterations, maxMean  perf tests only (Test/Perf.hpp)
//
// Names: a test is a type, so its name is CamelCase like any struct (readability-identifier-naming), and so is its
// suite, a namespace (GoogleTest's TEST(Suite, Name)). Both are string constants (SC_LIFT), part of the runner's
// cataloged format strings - no test name is a string in flash; the log label is "Suite.Name". Everything a test
// defines lives in namespace KvasirTests::<Suite>::<Name>.
#include "kvasir/Runtime/Check.hpp"
#include "kvasir/Runtime/Panic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_constant/string_constant.hpp>
#include <type_traits>

#ifndef UC_LOG_E   // a host test may bring its own capturing UC_LOG_* first
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Test {
/// How a test ends when it passes.
enum class Outcome : std::uint8_t {
    returns,   // the body returns
    panic,     // a panic with Options::cause (the image's panic reaction records and reboots)
    fault,   // a fault: a fault record, or a panic with Cause::fault (Fault::OnFault::Panic, the default)
    watchdog,   // the watchdog resets the chip: no panic or fault record, a watchdog reset kind (Config::Watchdog)
    reset,   // any reset will do (the body resets the chip itself)
};

struct Options {
    bool          enabled = true;
    Outcome       outcome = Outcome::returns;
    Panic::Cause  cause{};          // for Outcome::panic
    std::uint32_t iterations = 0;   // perf: 0 = Config::perfIterations
    std::uint32_t maxMean
      = 0;   // perf: the budget for the mean, in the perf clock's unit; 0 = none
};

/// A test's verdict, two bits a test in the run state.
enum class Verdict : std::uint8_t { none, pass, fail, skip };

/// What a test body gets: its index in the image's list and its failures. One per run of a body, never stored.
class Context {
public:
    constexpr explicit Context(std::uint16_t index) : index_{index} {}

    [[nodiscard]] constexpr std::uint16_t index() const { return index_; }

    constexpr void fail() { ++failures_; }

    constexpr void skip() { skipped_ = true; }

    [[nodiscard]] constexpr std::uint32_t failures() const { return failures_; }

    [[nodiscard]] constexpr bool skipped() const { return skipped_; }

    [[nodiscard]] constexpr Verdict verdict() const {
        if(skipped_) { return Verdict::skip; }
        return failures_ != 0 ? Verdict::fail : Verdict::pass;
    }

private:
    std::uint16_t index_;
    std::uint32_t failures_{};
    bool          skipped_{};
};

/// A perf test's loop: `while(perf.keepRunning()) { ... }`. Iteration 0 is a warm-up (caches, XIP, branch
/// predictors) and not counted; then `iterations` samples, each the time from one keepRunning() to the next, read
/// from the source the runner hands in (Config::Cycles: DWT cycles, or microseconds). Differences are taken modulo
/// 2^32, so a counter that wraps during a sample still gives its length.
class PerfState {
public:
    using Source = std::uint32_t (*)();

    constexpr PerfState(std::uint32_t iterations,
                        Source        now)
      : iterations_{iterations}
      , now_{now} {}

    /// true while the body should run another iteration.
    constexpr bool keepRunning() {
        auto const t = now_();
        if(calls_ >= 2) { record(t - last_); }
        last_ = t;
        ++calls_;
        return calls_ <= iterations_ + 1;
    }

    [[nodiscard]] constexpr std::uint32_t samples() const { return samples_; }

    [[nodiscard]] constexpr std::uint32_t min() const { return samples_ == 0 ? 0 : min_; }

    [[nodiscard]] constexpr std::uint32_t max() const { return max_; }

    /// The mean, rounded down; 0 without samples.
    [[nodiscard]] constexpr std::uint32_t mean() const {
        return samples_ == 0 ? 0 : static_cast<std::uint32_t>(total_ / samples_);
    }

private:
    constexpr void record(std::uint32_t d) {
        if(samples_ == 0 || d < min_) { min_ = d; }
        max_ = std::max(max_, d);
        total_ += d;
        ++samples_;
    }

    std::uint32_t iterations_;
    Source        now_;
    std::uint32_t calls_{};
    std::uint32_t last_{};
    std::uint32_t samples_{};
    std::uint32_t min_{};
    std::uint32_t max_{};
    std::uint64_t total_{};
};

/// A test, as the macros define it: suite and name (string constants), options, `perf`, and its body.
template<typename T>
concept TestCase = requires(Context& c, PerfState& p) {
    T::suite;
    T::name;
    { T::options } -> std::convertible_to<Options const&>;
    { T::isPerf } -> std::convertible_to<bool>;
    T::run(c, p);
};

/// A list of tests and suites (a suite is a List): flattened in order, so a test's index is its position.
template<typename... Ts>
struct List {};

/// What the runner found at the boot after a test that may not return: the evidence its Outcome is judged by.
struct Evidence {
    bool          panic{};   // a panic record was there
    Panic::Cause  cause{};   // its cause, pc and detail
    std::uint32_t pc{};
    std::uint32_t detail{};
    bool          fault{};      // a fault record was there
    bool          watchdog{};   // the reset was the watchdog's (timer or forced)
};

/// A test's second half, run at the boot after a cross-reset test once its outcome is judged (it may add failures):
///     KVASIR_AFTER_RESET(Panic, Assertion) { KVASIR_EXPECT_NE(evidence.pc, 0U); }
template<typename T>
struct AfterReset;

/// Keeps the optimiser from knowing a value (a division by an opaque zero, an assert it cannot fold away).
template<typename T>
[[gnu::noinline]] T opaque(T v) {
    asm volatile("" : "+r"(v));
    return v;
}

namespace Detail {
    template<typename... Ts>
    struct Concat;

    template<>
    struct Concat<> {
        using type = List<>;
    };

    template<typename... As>
    struct Concat<List<As...>> {
        using type = List<As...>;
    };

    template<typename... As, typename... Bs, typename... Rest>
    struct Concat<List<As...>, List<Bs...>, Rest...> {
        using type = typename Concat<List<As..., Bs...>, Rest...>::type;
    };

    template<typename T>
    struct Flatten {
        static_assert(TestCase<T>,
                      "Kvasir::Test::List<...>: an element is neither a test (KVASIR_TEST...) nor "
                      "a List / suite "
                      "(KVASIR_TEST_SUITE)");
        using type = List<T>;
    };

    template<typename... Ts>
    struct Flatten<List<Ts...>> {
        using type = typename Concat<typename Flatten<Ts>::type...>::type;
    };

    /// Whether a test has a second half (KVASIR_AFTER_RESET).
    template<typename T>
    inline constexpr bool hasAfterReset
      = requires(Context& c, Evidence const& e) { AfterReset<T>::run(c, e); };

    /// What a run of a body gives the runner.
    struct Result {
        std::uint32_t failures;
        bool          skipped;
    };

    /// One test's slot in the image: Slot<I, T>::run is a symbol of its own with the index in its mangled name
    /// (_ZN6Kvasir4Test6Detail4SlotILj<I>EN11KvasirTests<suite><name>EE3run...), which is how kvasir_test.py knows
    /// the list without booting the image.
    template<unsigned I, typename T>
    struct Slot {
        [[gnu::noinline,
          gnu::used]] static Result
        run(PerfState& perf) {
            Context c{static_cast<std::uint16_t>(I)};
            T::run(c, perf);
            return {c.failures(), c.skipped()};
        }
    };

    // The KVASIR_SKIP line: its reason is a string constant (cataloged, braces escaped), the index the context's.
    template<typename Reason>
    void logSkipped([[maybe_unused]] std::uint16_t index,
                    Reason) {
        UC_LOG_W(SC_LIFT("TEST|SKIPPED|{}|") + Reason{}, index);
    }
}   // namespace Detail
}   // namespace Kvasir::Test

// One test. The body's code is kept whether the test is listed or not, so the host tool finds a test that was defined
// but forgotten in the list: used + retain (clang: retain keeps the section from --gc-sections); arm-none-eabi-gcc
// ignores retain with a warning there, and its used alone keeps the body.
#ifdef __clang__
    #define KVASIR_TEST_DETAIL_KEEP gnu::used, gnu::retain
#else
    #define KVASIR_TEST_DETAIL_KEEP gnu::used
#endif
#define KVASIR_TEST_WITH(suite_, name_, ...)                                   \
    namespace KvasirTests::suite_ {                                            \
        struct name_ {                                                         \
            static constexpr auto                    suite = SC_LIFT(#suite_); \
            static constexpr auto                    name  = SC_LIFT(#name_);  \
            static constexpr ::Kvasir::Test::Options options                   \
              = ::Kvasir::Test::Options              __VA_ARGS__;              \
            static constexpr bool                    isPerf = false;           \
            [[KVASIR_TEST_DETAIL_KEEP]] static void                            \
                        body(::Kvasir::Test::Context& kvasirTestContext);      \
            static void run(::Kvasir::Test::Context& c,                        \
                            ::Kvasir::Test::PerfState&) {                      \
                body(c);                                                       \
            }                                                                  \
        };                                                                     \
    }                                                                          \
    inline void KvasirTests::suite_::name_::body(                              \
      [[maybe_unused]] ::Kvasir::Test::Context& kvasirTestContext)

#define KVASIR_TEST(suite_, name_) KVASIR_TEST_WITH(suite_, name_, {})

/// A perf test (Test/Perf.hpp): the body loops `while(perf.keepRunning()) { ... }`; Options::iterations and maxMean.
#define KVASIR_PERF_TEST(suite_, name_, ...)                                   \
    namespace KvasirTests::suite_ {                                            \
        struct name_ {                                                         \
            static constexpr auto                    suite = SC_LIFT(#suite_); \
            static constexpr auto                    name  = SC_LIFT(#name_);  \
            static constexpr ::Kvasir::Test::Options options                   \
              = ::Kvasir::Test::Options              __VA_ARGS__;              \
            static constexpr bool                    isPerf = true;            \
            [[KVASIR_TEST_DETAIL_KEEP]] static void                            \
                        body(::Kvasir::Test::Context&   kvasirTestContext,     \
                             ::Kvasir::Test::PerfState& perf);                 \
            static void run(::Kvasir::Test::Context&   c,                      \
                            ::Kvasir::Test::PerfState& p) {                    \
                body(c, p);                                                    \
            }                                                                  \
        };                                                                     \
    }                                                                          \
    inline void KvasirTests::suite_::name_::body(                              \
      [[maybe_unused]] ::Kvasir::Test::Context& kvasirTestContext,             \
      ::Kvasir::Test::PerfState&                perf)

/// KvasirTests::<suite>::Suite = the tests named, in this order.
#define KVASIR_TEST_SUITE(suite_, ...)                                                 \
    namespace KvasirTests::suite_ { using Suite = ::Kvasir::Test::List<__VA_ARGS__>; }

/// The second half of a cross-reset test (Kvasir::Test::AfterReset): its body sees `evidence`.
#define KVASIR_AFTER_RESET(suite_, name_)                                  \
    template<>                                                             \
    struct Kvasir::Test::AfterReset<KvasirTests::suite_::name_> {          \
        static void run(::Kvasir::Test::Context&        kvasirTestContext, \
                        ::Kvasir::Test::Evidence const& evidence);         \
    };                                                                     \
    inline void Kvasir::Test::AfterReset<KvasirTests::suite_::name_>::run( \
      [[maybe_unused]] ::Kvasir::Test::Context&        kvasirTestContext,  \
      [[maybe_unused]] ::Kvasir::Test::Evidence const& evidence)

// The comparison: both operands evaluated once (Check's Hold), compared as Check compares (std::cmp_* for integers),
// logged with the expression text (braces escaped) on failure.
#define KVASIR_TEST_DETAIL_CMP(Cmp, opText, a, b, onFailure)                                       \
    do {                                                                                           \
        ::Kvasir::Check::Detail::Hold<decltype((a))> const kvasirTestHa{(a)};                      \
        ::Kvasir::Check::Detail::Hold<decltype((b))> const kvasirTestHb{(b)};                      \
        if(!::Kvasir::Check::Detail::Cmp{}(kvasirTestHa.value, kvasirTestHb.value)) [[unlikely]] { \
            kvasirTestContext.fail();                                                              \
            UC_LOG_E(SC_LIFT("TEST|EXPECT|{}|") + KVASIR_DETAIL_CHECK_TEXT(#a " " opText " " #b)   \
                       + SC_LIFT(" failed: {} vs {}"),                                             \
                     kvasirTestContext.index(),                                                    \
                     kvasirTestHa.value,                                                           \
                     kvasirTestHb.value);                                                          \
            onFailure                                                                              \
        }                                                                                          \
    } while(false)

#define KVASIR_TEST_DETAIL_COND(cond, onFailure)                                  \
    do {                                                                          \
        if(!static_cast<bool>(cond)) [[unlikely]] {                               \
            kvasirTestContext.fail();                                             \
            UC_LOG_E(SC_LIFT("TEST|EXPECT|{}|") + KVASIR_DETAIL_CHECK_TEXT(#cond) \
                       + SC_LIFT(" failed"),                                      \
                     kvasirTestContext.index());                                  \
            onFailure                                                             \
        }                                                                         \
    } while(false)

// |a - b| <= eps without an unsigned wrap: the larger minus the smaller.
#define KVASIR_TEST_DETAIL_NEAR(a, b, eps, onFailure)                                         \
    do {                                                                                      \
        ::Kvasir::Check::Detail::Hold<decltype((a))> const   kvasirTestHa{(a)};               \
        ::Kvasir::Check::Detail::Hold<decltype((b))> const   kvasirTestHb{(b)};               \
        ::Kvasir::Check::Detail::Hold<decltype((eps))> const kvasirTestHe{(eps)};             \
        auto const kvasirTestDiff = kvasirTestHa.value < kvasirTestHb.value                   \
                                    ? kvasirTestHb.value - kvasirTestHa.value                 \
                                    : kvasirTestHa.value - kvasirTestHb.value;                \
        if(!::Kvasir::Check::Detail::Le{}(kvasirTestDiff, kvasirTestHe.value)) [[unlikely]] { \
            kvasirTestContext.fail();                                                         \
            UC_LOG_E(SC_LIFT("TEST|EXPECT|{}|")                                               \
                       + KVASIR_DETAIL_CHECK_TEXT(#a " ~= " #b " +- " #eps)                   \
                       + SC_LIFT(" failed: {} vs {}"),                                        \
                     kvasirTestContext.index(),                                               \
                     kvasirTestHa.value,                                                      \
                     kvasirTestHb.value);                                                     \
            onFailure                                                                         \
        }                                                                                     \
    } while(false)

#define KVASIR_TEST_DETAIL_GO_ON
#define KVASIR_TEST_DETAIL_END return;

#define KVASIR_EXPECT(cond)         KVASIR_TEST_DETAIL_COND(cond, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_EQ(a, b)      KVASIR_TEST_DETAIL_CMP(Eq, "==", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_NE(a, b)      KVASIR_TEST_DETAIL_CMP(Ne, "!=", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_LT(a, b)      KVASIR_TEST_DETAIL_CMP(Lt, "<", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_LE(a, b)      KVASIR_TEST_DETAIL_CMP(Le, "<=", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_GT(a, b)      KVASIR_TEST_DETAIL_CMP(Gt, ">", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_GE(a, b)      KVASIR_TEST_DETAIL_CMP(Ge, ">=", a, b, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_EXPECT_NEAR(a, b, e) KVASIR_TEST_DETAIL_NEAR(a, b, e, KVASIR_TEST_DETAIL_GO_ON)
#define KVASIR_REQUIRE(cond)        KVASIR_TEST_DETAIL_COND(cond, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_EQ(a, b)     KVASIR_TEST_DETAIL_CMP(Eq, "==", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_NE(a, b)     KVASIR_TEST_DETAIL_CMP(Ne, "!=", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_LT(a, b)     KVASIR_TEST_DETAIL_CMP(Lt, "<", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_LE(a, b)     KVASIR_TEST_DETAIL_CMP(Le, "<=", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_GT(a, b)     KVASIR_TEST_DETAIL_CMP(Gt, ">", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_GE(a, b)     KVASIR_TEST_DETAIL_CMP(Ge, ">=", a, b, KVASIR_TEST_DETAIL_END)
#define KVASIR_REQUIRE_NEAR(a, b, e) KVASIR_TEST_DETAIL_NEAR(a, b, e, KVASIR_TEST_DETAIL_END)

/// Skip this test (a part that is not fitted, a board without the wiring), ending it: "TEST|SKIPPED|<index>|<reason>".
#define KVASIR_SKIP(reason)                                                   \
    do {                                                                      \
        kvasirTestContext.skip();                                             \
        ::Kvasir::Test::Detail::logSkipped(kvasirTestContext.index(),         \
                                           KVASIR_DETAIL_CHECK_TEXT(reason)); \
        return;                                                               \
    } while(false)
