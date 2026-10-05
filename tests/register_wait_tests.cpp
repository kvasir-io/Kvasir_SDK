// Kvasir::Register::waitUntil (Register/Wait.hpp) on the register mock: a modelled register that turns true
// after k reads; Polls / Microseconds / Until bounds; the Report, Count and Panic policies; the injected policy
// specialised after the first use still wins; Unbounded counts nothing.
#include "kvasir/Register/Wait.hpp"
#include "test_registers.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace Kvasir::Panic {
[[noreturn,
  gnu::noinline]] void
raise(Cause cause) {
    Detail::dispatch(Info{cause, 0});
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

struct TestHandler {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        seen = info;
        std::longjmp(back, 1);
    }
};

// a clock that a test moves by hand
struct FakeClock {
    using time_point = std::uint32_t;
    static inline time_point t{};

    static time_point now() { return t; }
};

// the register turns true (bit 0, CtrlReg::en) on read number `after`
void becomesTrueAfter(unsigned after) {
    auto& m  = recorder.model(CtrlReg::Addr::value);
    m.onRead = [after, n = 0U](unsigned) mutable { return ++n >= after ? 1U : 0U; };
}

// at compile time: 1000 us at 48 MHz = 48 000 cycles / 4 per poll = 12 000 polls, x2 margin
static_assert(Bound::Microseconds<1'000,
                                  48'000'000>::polls
              == 24'000);
static_assert(Bound::Polls<7>::polls == 7);
}   // namespace

// the injected wait policy, specialised after Wait.hpp was included (and before the first instantiation of a
// waitUntil that uses it)
template<>
inline constexpr auto Kvasir::Panic::injectedHandler<> = TestHandler{};
template<>
inline constexpr auto Kvasir::Register::injectedWaitPolicy<> = OnTimeout::Report{};

// TSan's longjmp interceptor aborts on a jump out of a function that does not return; this test has no threads for
// TSan to judge.
#if defined(__SANITIZE_THREAD__)
    #define WAIT_TEST_NO_LONGJMP 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define WAIT_TEST_NO_LONGJMP 1
    #endif
#endif

int main() {
    test("isSet: true on the 5th read, Report");
    becomesTrueAfter(5);
    auto const a = waitUntil<Bound::Polls<100>, OnTimeout::Report>(isSet(CtrlReg::en));
    CHECK(a && a.polls == 5);
    CHECK_EQ(readCount(CtrlReg::Addr::value), 5U);

    test("isClear and a callable");
    {
        auto& m      = recorder.model(CtrlReg::Addr::value);
        m.value      = 1;
        auto const b = waitUntil<Bound::Polls<3>, OnTimeout::Report>(isClear(CtrlReg::en));
        CHECK(!b && b.timedOut && b.polls == 3);
        m.value      = 0;
        auto const c = waitUntil<Bound::Polls<3>, OnTimeout::Report>(isClear(CtrlReg::en));
        CHECK(c && c.polls == 1);
        int        calls = 0;
        auto const d = waitUntil<Bound::Polls<10>, OnTimeout::Report>([&] { return ++calls == 4; });
        CHECK(d && d.polls == 4 && calls == 4);
    }

    test("never true: exactly N reads, then the timeout");
    becomesTrueAfter(1'000'000);
    auto const e = waitUntil<Bound::Polls<50>, OnTimeout::Report>(isSet(CtrlReg::en));
    CHECK(e.timedOut && e.polls == 50);
    CHECK_EQ(readCount(CtrlReg::Addr::value), 50U);

    test("Count counts and carries on");
    OnTimeout::Count::timeouts = 0;
    becomesTrueAfter(1'000'000);
    auto const f = waitUntil<Bound::Polls<4>, OnTimeout::Count>(isSet(CtrlReg::en));
    CHECK(f.timedOut && OnTimeout::Count::timeouts == 1U);

    test("Until: the deadline, and the poll ceiling when the clock stands");
    {
        becomesTrueAfter(1'000'000);
        auto& m  = recorder.model(CtrlReg::Addr::value);
        auto  n  = 0U;
        m.onRead = [&n](unsigned) {
            FakeClock::t = FakeClock::t + 1;   // each read takes a tick
            ++n;
            return 0U;
        };
        FakeClock::t = 0;
        auto const g
          = waitUntil<Bound::Until<FakeClock>, OnTimeout::Report>(isSet(CtrlReg::en),
                                                                  Bound::Until<FakeClock>{10});
        CHECK(g.timedOut && FakeClock::t >= 10);
        m.onRead     = [](unsigned) { return 0U; };   // the clock stands still
        auto const h = waitUntil<Bound::Until<FakeClock, 20>, OnTimeout::Report>(
          isSet(CtrlReg::en),
          Bound::Until<FakeClock, 20>{FakeClock::t + 1000});
        CHECK(h.timedOut && h.polls == 20);
    }

#ifndef WAIT_TEST_NO_LONGJMP
    test("Panic: cause timeout, the register's address as the detail");
    becomesTrueAfter(1'000'000);
    seen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(back) == 0) {
        (void)waitUntil<Bound::Polls<2>, OnTimeout::Panic>(isSet(CtrlReg::en));
    }
    CHECK(seen && seen->cause == Kvasir::Panic::Cause::timeout);
    CHECK(seen && seen->detail == CtrlReg::Addr::value);
#endif

    test("the injected policy (Report here) is the one without a policy argument");
    becomesTrueAfter(1'000'000);
    auto const i = waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
    CHECK(i.timedOut && i.polls == 3);

    test("Unbounded: the loop, no count");
    becomesTrueAfter(3);
    auto const j = waitUntil<Bound::Polls<1>, OnTimeout::Unbounded>(isSet(CtrlReg::en));
    CHECK(j && j.polls == 0);
    CHECK_EQ(readCount(CtrlReg::Addr::value), 3U);

    return report();
}
