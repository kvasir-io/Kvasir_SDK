// Kvasir::Retry (Util/Retry.hpp): the first failure is due after `first`, the pause doubles up to `longest`, a success
// starts over, and a clock that jumps far makes it due once, not once per missed pause.
#include "kvasir/Util/Retry.hpp"

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>

namespace {
struct TestClock {
    using rep        = std::int64_t;
    using period     = std::milli;
    using duration   = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<TestClock>;
};

enum class E : std::uint8_t { a, b };

struct Short {
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::seconds longest{5};
};

using ms = std::chrono::milliseconds;
}   // namespace

int main() {
    using Kvasir::Test::test;
    using R = Kvasir::Retry<TestClock, E, Short>;
    TestClock::time_point t{};

    {
        test("nothing pending: never due");
        R r{};
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
        static_assert(D::First == ms{1000} && D::Longest == ms{600'000});
        D r{};
        for(int i = 0; i != 20; ++i) { r.failed(E::a, t); }
        CHECK(r.pause == ms{600'000});
    }

    return Kvasir::Test::report();
}
