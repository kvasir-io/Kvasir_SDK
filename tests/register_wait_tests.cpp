// Kvasir::Register::waitUntil (Register/Wait.hpp) on the register mock: a modelled register that turns true
// after k reads; Polls / Microseconds / Until bounds; the Report, Count and Panic policies; the image's policy
// specialised after the first use still wins; Unbounded counts nothing; MicrosecondsBound over the specialised
// CpuHzCeiling<Tag::User>.
#include "harness/PanicCatcher.hpp"
#include "kvasir/Register/Wait.hpp"
#include "test_registers.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>
#include <type_traits>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
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

// the image's wait policy (the hook), specialised after Wait.hpp was included (and before the first instantiation of a
// waitUntil that uses it)
template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::CatchPanic;
};

template<>
struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> {
    using type = OnTimeout::Report;
};

// what a chip package's chip/WaitBounds.hpp does with its CpuHzCeiling
template<>
struct Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User> {
    static constexpr std::uint64_t value = 48'000'000;
};

static_assert(MicrosecondsBound<1'000>::polls
              == Bound::Microseconds<1'000,
                                     48'000'000>::polls);
static_assert(std::is_same_v<MicrosecondsBound<1'000>,
                             Bound::Microseconds<1'000,
                                                 48'000'000>>);

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

#ifndef KVASIR_TEST_NO_LONGJMP
    test("Panic: cause timeout, the register's address as the detail");
    becomesTrueAfter(1'000'000);
    panicSeen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(panicBack) == 0) {
        (void)waitUntil<Bound::Polls<2>, OnTimeout::Panic>(isSet(CtrlReg::en));
    }
    CHECK(panicSeen && panicSeen->cause == Kvasir::Panic::Cause::timeout);
    CHECK(panicSeen && panicSeen->detail == CtrlReg::Addr::value);
#endif

    test("the image's policy (Report here) is the one without a policy argument");
    becomesTrueAfter(1'000'000);
    auto const i = waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
    CHECK(i.timedOut && i.polls == 3);

    test(
      "FieldValue on a whole 32-bit register: the one-hot SELECTED shape (chip_rp_common "
      "clock_config.hpp)");
    {
        using Full = Kvasir::Register::FieldLocation<CtrlReg::Addr,
                                                     Kvasir::Register::maskFromRange(31, 0),
                                                     Kvasir::Register::ReadOnlyAccess,
                                                     std::uint32_t>;
        constexpr auto selectedIs2 = Kvasir::Register::FieldValue<Full, (std::uint32_t{1} << 2)>{};
        static_assert(std::is_same_v<decltype(selectedIs2) const,
                                     decltype(Kvasir::Register::FieldValue<Full, 4U>{}) const>);
        auto& m = recorder.model(CtrlReg::Addr::value);
        auto  n = 0U;
        // "may briefly show all-0s" while switching, then the new source's bit; the old one first
        m.onRead     = [&n](unsigned) { return ++n < 3 ? 1U : (n < 5 ? 0U : 4U); };
        auto const k = waitUntil<Bound::Polls<10>, OnTimeout::Report>(selectedIs2);
        CHECK(k && k.polls == 5);
        m.onRead     = [](unsigned) { return 5U; };   // two bits: not the value
        auto const l = waitUntil<Bound::Polls<4>, OnTimeout::Report>(selectedIs2);
        CHECK(l.timedOut);
        m.onRead = nullptr;
    }

    test("Unbounded: the loop, no count");
    becomesTrueAfter(3);
    auto const j = waitUntil<Bound::Polls<1>, OnTimeout::Unbounded>(isSet(CtrlReg::en));
    CHECK(j && j.polls == 0);
    CHECK_EQ(readCount(CtrlReg::Addr::value), 3U);

    return report();
}
