// kvasir/Log/FaultLog.hpp: the key is rateLimitKey(kind, key...), the decision is the limiter's, the counts are
// per kind and saturate, and without counts a FaultLog is the size of its limiter (empty with NoRateLimiter).
#include "kvasir/Log/FaultLog.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace {
using TestClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

enum class Fault : std::uint8_t { timeout, overrun, nak };

using Real
  = Kvasir::FaultLog<TestClock, Fault, Kvasir::RateLimiterConfig{}, 0, Kvasir::RateLimiter>;
using Counted
  = Kvasir::FaultLog<TestClock, Fault, Kvasir::RateLimiterConfig{}, 3, Kvasir::RateLimiter>;
using Silent
  = Kvasir::FaultLog<TestClock, Fault, Kvasir::RateLimiterConfig{}, 0, Kvasir::NoRateLimiter>;

static_assert(sizeof(Real) == sizeof(Kvasir::RateLimiter<TestClock>),
              "no counts: just the limiter");
static_assert(sizeof(Counted) > sizeof(Real),
              "counts take their bytes");

struct Holder {
    [[no_unique_address]] Silent log{};
    std::uint32_t                other{};
};

static_assert(std::is_empty_v<Silent> && sizeof(Holder) == sizeof(std::uint32_t),
              "with NoRateLimiter and no counts nothing is left");

template<typename L>
concept HasCount = requires(L const& l) { l.count(Fault::timeout); };
static_assert(HasCount<Counted> && !HasCount<Real>,
              "count() only where counted");
}   // namespace

int main() {
    using Kvasir::Test::test;
    TestClock::time_point const t{};

    {
        test("note() is the limiter's allow() on rateLimitKey(kind, key...)");
        Real                           a{};
        Kvasir::RateLimiter<TestClock> b{};
        for(int i = 0; i < 50; ++i) {
            auto const now = t + std::chrono::milliseconds{i * 37};
            auto const da  = a.note(Fault::overrun, now, std::uint8_t{3}, i % 2 == 0);
            auto const db
              = b.allow(Kvasir::rateLimitKey(Fault::overrun, std::uint8_t{3}, i % 2 == 0), now);
            CHECK(da.allowed == db.allowed && da.suppressed == db.suppressed);
        }
        CHECK_EQ(a.takeSummary(t + std::chrono::hours{1}),
                 b.takeSummary(t + std::chrono::hours{1}));
    }
    {
        test("counts per kind, saturating at 65535");
        Counted c{};
        for(int i = 0; i < 1000; ++i) { static_cast<void>(c.note(Fault::timeout, t)); }
        static_cast<void>(c.note(Fault::nak, t, 1));
        CHECK_EQ(c.count(Fault::timeout), 1000U);
        CHECK_EQ(c.count(Fault::nak), 1U);
        CHECK_EQ(c.count(Fault::overrun), 0U);
        for(int i = 0; i < 70000; ++i) { static_cast<void>(c.note(Fault::overrun, t)); }
        CHECK_EQ(c.count(Fault::overrun), 65535U);
    }
    {
        test("NoRateLimiter: never allows, no summary");
        Silent s{};
        CHECK(!s.note(Fault::timeout, t).allowed);
        CHECK_EQ(s.takeSummary(t + std::chrono::hours{1}), 0U);
    }
    return Kvasir::Test::report();
}
