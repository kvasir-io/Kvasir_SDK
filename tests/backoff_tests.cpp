// Kvasir::Backoff and Kvasir::doubledUpTo (Util/Backoff.hpp): the schedule 1, 2, 4 ... up to the ceiling, a success
// starts over, a jumped clock makes it due once, and the step at its edges (an odd ceiling, d == longest / 2, a 32-bit
// unsigned millisecond count near its top).
#include "kvasir/Util/Backoff.hpp"

#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <ratio>

namespace {
using TestClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;
using ms        = std::chrono::milliseconds;
using Millis32  = std::chrono::duration<std::uint32_t, std::milli>;

struct Short {
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::seconds longest{8};
};

static_assert(Kvasir::doubledUpTo(ms{1},
                                  ms{8})
              == ms{2});
static_assert(Kvasir::doubledUpTo(ms{4},
                                  ms{8})
                == ms{8},
              "d == longest / 2: the ceiling");
static_assert(Kvasir::doubledUpTo(ms{5},
                                  ms{8})
              == ms{8});
static_assert(Kvasir::doubledUpTo(ms{8},
                                  ms{8})
                == ms{8},
              "stays at the ceiling");
static_assert(Kvasir::doubledUpTo(ms{3},
                                  ms{7})
                == ms{7},
              "odd ceiling: 3 >= 7 / 2");
static_assert(Kvasir::doubledUpTo(ms{2},
                                  ms{7})
              == ms{4});
// no overflow near the top of an unsigned 32-bit count: 2^31 + 1 doubled would wrap
static_assert(Kvasir::doubledUpTo(Millis32{2'147'483'649U},
                                  Millis32{4'294'967'295U})
              == Millis32{4'294'967'295U});
static_assert(Kvasir::doubledUpTo(Millis32{1'000'000'000U},
                                  Millis32{4'294'967'295U})
              == Millis32{2'000'000'000U});
}   // namespace

int main() {
    using Kvasir::Test::test;
    using B = Kvasir::Backoff<TestClock, Short>;
    TestClock::time_point t{};

    {
        test("no failure: never due");
        B const b{};
        CHECK(!b.due(t) && !b.due(t + std::chrono::hours{1}));
    }
    {
        test("the schedule doubles up to the ceiling: 1, 2, 4, 8, 8 s");
        B                               b{};
        std::chrono::milliseconds const want[] = {ms{1000}, ms{2000}, ms{4000}, ms{8000}, ms{8000}};
        for(auto const w : want) {
            b.failed(t);
            CHECK(!b.due(t + w - ms{1}) && b.due(t + w));
            t += w;
        }
        CHECK_EQ(b.failures, 5U);
    }
    {
        test("a success starts it over");
        B b{};
        b.failed(t);
        b.failed(t);
        b.succeeded();
        CHECK(!b.due(t + std::chrono::hours{1}) && b.failures == 0 && b.pause == B::first);
        b.failed(t);
        CHECK(b.due(t + ms{1000}) && b.pause == ms{2000});
    }
    {
        test("a jumped clock makes it due once");
        B b{};
        b.failed(t);
        auto const later = t + std::chrono::hours{5};
        CHECK(b.due(later));
        b.failed(later);
        CHECK(!b.due(later + ms{1999}) && b.due(later + ms{2000}));
    }
    return Kvasir::Test::report();
}
