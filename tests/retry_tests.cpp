// Kvasir::Retry (Util/Retry.hpp, a Backoff plus the pending value): the first failure is due after `first`, the pause
// doubles up to `longest`, a success starts over, and a clock that jumps far makes it due once, not once per missed
// pause.
#include "kvasir/Util/Retry.hpp"

#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>

namespace {
using TestClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

enum class E : std::uint8_t { a, b };

struct Short {
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::seconds longest{5};
};

struct One {   // absent after the first failure
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::seconds longest{5};
    static constexpr std::uint16_t        absentAfter = 1;
};

using ms = std::chrono::milliseconds;

// a Retry is its Backoff plus pending/last, which sit in the Backoff's tail padding: no byte more than the Backoff
static_assert(sizeof(Kvasir::Retry<TestClock,
                                   E,
                                   Short>)
              == sizeof(Kvasir::Backoff<TestClock,
                                        Short>));
}   // namespace

int main() {
    using Kvasir::Test::test;
    using R = Kvasir::Retry<TestClock, E, Short>;
    TestClock::time_point t{};

    {
        test("nothing pending: never due");
        R const r{};
        CHECK(!r.pending && !r.due(t) && !r.due(t + std::chrono::hours{1}));
    }

    {
        test("a failure is due after the first pause, the pause doubles and is capped");
        R r{};
        r.failed(E::b, t);
        CHECK(r.pending && r.last == E::b && r.failures == 1);
        CHECK(!r.due(t + ms{999}) && r.due(t + ms{1000}));
        t += ms{1000};
        r.failed(E::a, t);   // next after 2 s
        CHECK(!r.due(t + ms{1999}) && r.due(t + ms{2000}) && r.last == E::a);
        t += ms{2000};
        r.failed(E::a, t);   // 4 s
        CHECK(r.next == t + ms{4000});
        t += ms{4000};
        r.failed(E::a, t);   // 5 s, the ceiling
        CHECK(r.next == t + ms{5000} && r.pause == ms{5000});
        t += ms{5000};
        r.failed(E::a, t);
        CHECK(r.next == t + ms{5000} && r.failures == 5);
    }

    {
        test("a success starts over");
        R r{};
        r.failed(E::a, t);
        r.failed(E::a, t);
        r.succeeded();
        CHECK(!r.pending && r.failures == 0 && r.pause == ms{1000});
        r.failed(E::a, t);
        CHECK(r.next == t + ms{1000});
    }

    {
        test(
          "a clock that jumps an hour: due once, and the next pause is the doubled one, not a "
          "burst");
        R r{};
        r.failed(E::a, t);
        auto const late = t + std::chrono::hours{1};
        CHECK(r.due(late));
        r.failed(E::a, late);
        CHECK(!r.due(late) && !r.due(late + ms{1999}) && r.due(late + ms{2000}));
    }

    {
        test("the defaults: 1 s, 10 min");
        using D = Kvasir::Retry<TestClock, E>;
        static_assert(D::first == ms{1000} && D::longest == ms{600'000});
        D r{};
        for(int i = 0; i != 20; ++i) { r.failed(E::a, t); }
        CHECK(r.pause == ms{600'000});
    }

    {
        test(
          "linkOf: answering while nothing is pending, starting below absentAfter (3) failures, "
          "absent from it on");
        using D = Kvasir::Retry<TestClock, E>;
        D r{};
        CHECK(Kvasir::linkOf(r) == Kvasir::Link::answering);
        r.failed(E::a, t);
        CHECK(Kvasir::linkOf(r) == Kvasir::Link::starting);
        r.failed(E::a, t);
        CHECK(Kvasir::linkOf(r) == Kvasir::Link::starting);
        r.failed(E::a, t);
        CHECK(Kvasir::linkOf(r) == Kvasir::Link::absent);
        r.succeeded();
        CHECK(Kvasir::linkOf(r) == Kvasir::Link::answering);
        test("linkOf: a Config's absentAfter");
        Kvasir::Retry<TestClock, E, One> o{};
        o.failed(E::a, t);
        CHECK(Kvasir::linkOf(o) == Kvasir::Link::absent);
        CHECK(Kvasir::linkOf(R{}) == Kvasir::Link::answering);   // a Config without it: the default
    }

    return Kvasir::Test::report();
}
