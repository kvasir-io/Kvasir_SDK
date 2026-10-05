// Kvasir::Check (Util/Check.hpp) on the host, without uc_log's output (the log lines compile to their
// no-log form): each check passes and fails on the right side, integers compare by value, every operand is
// evaluated exactly once, a bit-field and a volatile operand work, a prvalue operand lives through the
// compare, the cause is checkFailed, a soft check returns false and runs the injected policy, and a passing
// check works in a constant evaluation.
#include "kvasir/Util/Check.hpp"

#include "test_harness.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>
#include <string>

namespace Kvasir::Panic {
[[noreturn,
  gnu::noinline]] void
raise(Cause cause) {
    Detail::dispatch(Info{
      cause,
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)))});
}

[[noreturn]] void raiseAt(Cause         cause,
                          std::uint32_t pc,
                          std::uint32_t detail) {
    Detail::dispatch(Info{cause, pc, detail});
}
}   // namespace Kvasir::Panic

namespace {
std::jmp_buf                       back;
std::optional<Kvasir::Panic::Info> seen;
int                                softFailures = 0;

struct TestHandler {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        seen = info;
        std::longjmp(back, 1);
    }
};

struct CountingSoftPolicy {
    static void operator()() { ++softFailures; }
};
}   // namespace

template<>
inline constexpr auto Kvasir::Panic::injectedHandler<> = TestHandler{};
template<>
inline constexpr auto Kvasir::Check::injectedSoftCheckPolicy<> = CountingSoftPolicy{};

namespace {
using Kvasir::Panic::Cause;

#if defined(__SANITIZE_THREAD__)
    #define CHECK_TEST_NO_LONGJMP 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define CHECK_TEST_NO_LONGJMP 1
    #endif
#endif

// Whether `f` panics, and with which cause.
template<typename F>
std::optional<Cause> panicOf(F&& f) {
    seen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(back) == 0) { f(); }
    return seen ? std::optional<Cause>{seen->cause} : std::nullopt;
}

enum class Mode : std::uint8_t { idle, run, invalid };

struct Bits {
    unsigned slot : 3;
};

int calls = 0;

int counted(int v) {
    ++calls;
    return v;
}

// a passing check in a constant evaluation compiles
constexpr int checked(int v) {
    KVASIR_CHECK_LE(v, 100);
    KVASIR_CHECK(v >= 0, "v {}", v);
    return v;
}

static_assert(checked(30) == 30);
}   // namespace

int main() {
    using Kvasir::Test::test;
#ifdef CHECK_TEST_NO_LONGJMP
    return 0;
#endif

    test("passing checks do nothing");
    CHECK(!panicOf([] {
        KVASIR_CHECK_LE(64, 96);
        KVASIR_CHECK_EQ(3U, 3);
        KVASIR_CHECK_NE(Mode::run, Mode::invalid);
        KVASIR_CHECK_LT(-1, 1U);   // by value: -1 < 1
        KVASIR_CHECK_GE(std::string{"b"}, std::string{"a"});
        KVASIR_CHECK(true, "never {}", 1);
        KVASIR_DCHECK_GT(2, 1);
    }));

    test("failing checks panic with checkFailed");
    CHECK(panicOf([] { KVASIR_CHECK_LE(96, 64, "fifo {} overflow", 2); }) == Cause::checkFailed);
    CHECK(panicOf([] { KVASIR_CHECK_NE(Mode::invalid, Mode::invalid); }) == Cause::checkFailed);
    CHECK(panicOf([] { KVASIR_CHECK_EQ(-1, 0xFFFF'FFFFU); })
          == Cause::checkFailed);   // not the bits
    CHECK(panicOf([] { KVASIR_CHECK(false); }) == Cause::checkFailed);
    CHECK(panicOf([] { KVASIR_CHECK_UNREACHABLE("state {}", 3); }) == Cause::checkFailed);
    CHECK(panicOf([] { KVASIR_DCHECK_LT(5, 4); }) == Cause::checkFailed);

    test("a bit-field and a volatile operand");
    {
        Bits b{.slot = 6};
        CHECK(panicOf([&] { KVASIR_CHECK_LT(b.slot, 4U); }) == Cause::checkFailed);
        CHECK(!panicOf([&] { KVASIR_CHECK_GT(b.slot, 4U); }));
        static int volatile v = 7;
        CHECK(!panicOf([] { KVASIR_CHECK_EQ(v, 7); }));
    }

    test("every operand evaluated exactly once");
    calls = 0;
    CHECK(!panicOf([] { KVASIR_CHECK_LT(counted(1), counted(2)); }));
    CHECK(calls == 2);
    calls = 0;
    CHECK(panicOf([] { KVASIR_CHECK_GT(counted(1), counted(2)); }) == Cause::checkFailed);
    CHECK(calls == 2);

    test("a prvalue operand lives through the compare (ASan)");
    CHECK(!panicOf([] { KVASIR_CHECK_EQ(std::string(100, 'x').size(), 100U); }));
    CHECK(!panicOf([] { KVASIR_CHECK_EQ(std::string(100, 'x'), std::string(100, 'x')); }));

    test("a soft check returns false and runs the policy, without a panic");
    softFailures = 0;
    bool kept    = true;
    CHECK(!panicOf([&] { kept = KVASIR_SOFT_CHECK(1 == 1); }));
    CHECK(kept && softFailures == 0);
    CHECK(!panicOf([&] { kept = KVASIR_SOFT_CHECK(96 <= 64, "dropping {} bytes", 32); }));
    CHECK(!kept && softFailures == 1);

    return Kvasir::Test::report();
}
