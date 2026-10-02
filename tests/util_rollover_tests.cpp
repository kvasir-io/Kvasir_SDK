// Rollover<T>: distances and the half-window comparisons across the wrap, for every width, and the
// deleted relational operators. Almost everything is checked at compile time.
#include "kvasir/Util/Rollover.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

using Kvasir::after;
using Kvasir::before;
using Kvasir::inWindow;
using Kvasir::Rollover;

namespace {

template<typename R>
concept LessComparable = requires(R a, R b) { a < b; };
template<typename R>
concept GreaterComparable = requires(R a, R b) { a > b; };
template<typename R>
concept LessEqualComparable = requires(R a, R b) { a <= b; };
template<typename R>
concept GreaterEqualComparable = requires(R a, R b) { a >= b; };

// width-generic checks: one step across the wrap, both directions, and the half-range edge
template<typename T,
         unsigned Bits = sizeof(T) * 8>
constexpr bool wrapsRight() {
    using R         = Rollover<T, Bits>;
    constexpr T top = R::mask;   // the last value before the wrap
    R const     last{top};
    R const     first{0};
    R const     halfway{static_cast<T>(T{1} << (Bits - 1))};

    bool ok = true;
    ok      = ok && (first - last) == 1;
    ok      = ok && (last - first) == -1;
    ok      = ok && before(last, first) && after(first, last);
    ok      = ok && !before(first, first) && !after(first, first);
    ok      = ok && last + 1 == first && first - 1 == last;
    // exactly half the range apart: each is "before" the other (the documented edge)
    ok = ok && before(halfway, first) && before(first, halfway);
    ok = ok
      && (halfway - first)
           == std::numeric_limits<std::make_signed_t<T>>::min() >> (sizeof(T) * 8 - Bits);
    // the value is masked to Bits
    ok = ok && R{static_cast<T>(~T{0})}.value() == top;
    ok = ok && !LessComparable<R> && !GreaterComparable<R> && !LessEqualComparable<R>
      && !GreaterEqualComparable<R>;
    ok = ok && std::is_trivially_copyable_v<R> && sizeof(R) == sizeof(T);
    return ok;
}

static_assert(wrapsRight<std::uint8_t>());
static_assert(wrapsRight<std::uint16_t>());
static_assert(wrapsRight<std::uint32_t>());
static_assert(wrapsRight<std::uint64_t>());
static_assert(wrapsRight<std::uint32_t,
                         24>());
static_assert(wrapsRight<std::uint8_t,
                         4>());

// the plan's example
static_assert(Rollover<std::uint32_t>{0x10U} - Rollover<std::uint32_t>{0xFFFF'FFF0U} == 32);
static_assert((Rollover<std::uint32_t>{0xFFFF'FFF0U} + 40).value() == 0x18U);
static_assert(before(Rollover<std::uint32_t>{0xFFFF'FFF0U},
                     Rollover<std::uint32_t>{0x10U}));

// 24-bit SysTick: a down counter, elapsed = start - now
static_assert(Rollover<std::uint32_t,
                       24>{0x10U}
                - Rollover<std::uint32_t,
                           24>{0xFF'FFF0U}
              == 32);
static_assert((Rollover<std::uint32_t,
                        24>{0xFF'FFFFU}
               + 1)
                .value()
              == 0U);

// example 39's stepsBetween, on the type: signed encoder counts through their unsigned images
constexpr std::int32_t stepsBetween(std::int32_t now,
                                    std::int32_t last) {
    return Rollover{static_cast<std::uint32_t>(now)} - Rollover{static_cast<std::uint32_t>(last)};
}

static_assert(stepsBetween(5,
                           2)
              == 3);
static_assert(stepsBetween(-2,
                           3)
              == -5);
static_assert(stepsBetween(-2'147'483'647 - 1,
                           2'147'483'647)
                == 1,
              "one step across the wrap");

// inWindow: a window that wraps and one that does not
using R16 = Rollover<std::uint16_t>;
static_assert(inWindow(R16{5},
                       R16{3},
                       R16{9}));
static_assert(inWindow(R16{3},
                       R16{3},
                       R16{9})
              && inWindow(R16{9},
                          R16{3},
                          R16{9}));
static_assert(!inWindow(R16{2},
                        R16{3},
                        R16{9})
              && !inWindow(R16{10},
                           R16{3},
                           R16{9}));
static_assert(inWindow(R16{0xFFFF},
                       R16{0xFFF0},
                       R16{0x0010}));
static_assert(inWindow(R16{0x0005},
                       R16{0xFFF0},
                       R16{0x0010}));
static_assert(!inWindow(R16{0x0011},
                        R16{0xFFF0},
                        R16{0x0010}));
static_assert(!inWindow(R16{0xFFEF},
                        R16{0xFFF0},
                        R16{0x0010}));
// a window over more than half the range still works
static_assert(inWindow(R16{0x9000},
                       R16{0x0000},
                       R16{0xF000}));

}   // namespace

// the run-time forms, so the operators are also exercised outside constant evaluation
int main() {
    using Kvasir::Test::test;
    test("rollover");

    std::uint32_t volatile raw = 0xFFFF'FFFEU;
    Rollover<std::uint32_t> a{raw};
    Rollover<std::uint32_t> b = a;
    b += 5;
    CHECK(b.value() == 3U);
    CHECK((b - a) == 5);
    CHECK(before(a, b));
    CHECK(after(b, a));
    auto const old = a++;
    CHECK(old.value() == 0xFFFF'FFFEU && a.value() == 0xFFFF'FFFFU);
    ++a;
    CHECK(a.value() == 0U);
    a -= 1;
    CHECK(a.value() == 0xFFFF'FFFFU);

    return Kvasir::Test::report();
}
