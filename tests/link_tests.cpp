// kvasir/Util/Link.hpp: FailureStreak saturates at 255 and is absent from N on (justAbsent only at N), its link(up)
// for the three cases; LinkTracker: starting -> answering (1 bring-up) -> N - 1 failures still answering -> the N-th
// absent -> up again (2 bring-ups).
#include "kvasir/Util/Link.hpp"

#include "test_harness.hpp"

#include <cstdint>

namespace {
using Kvasir::Link;
static_assert(sizeof(Kvasir::LinkTracker<3>) == 4,
              "Link, uint8_t, uint16_t: the four bytes a driver had");
static_assert(sizeof(Kvasir::FailureStreak<3>) == 1);
}   // namespace

int main() {
    using Kvasir::Test::test;
    {
        test("FailureStreak: absent from N, justAbsent at N, saturating");
        Kvasir::FailureStreak<3> s{};
        CHECK(!s.absent() && s.link(false) == Link::starting && s.link(true) == Link::answering);
        s.failed();
        s.failed();
        CHECK(!s.absent() && !s.justAbsent());
        s.failed();
        CHECK(s.absent() && s.justAbsent() && s.link(false) == Link::absent
              && s.link(true) == Link::answering);
        s.failed();
        CHECK(s.absent() && !s.justAbsent());
        for(int i = 0; i < 400; ++i) { s.failed(); }
        CHECK_EQ(s.n, 255U);
        s.cleared();
        CHECK(!s.absent() && s.n == 0);
    }
    {
        test("LinkTracker: NorFlash's semantics");
        Kvasir::LinkTracker<3> l{};
        CHECK(l.link() == Link::starting && l.bringUps() == 0);
        l.up();
        CHECK(l.link() == Link::answering && l.bringUps() == 1);
        l.up();
        CHECK(l.bringUps() == 1);
        l.failed();
        l.failed();
        CHECK(l.link() == Link::answering && l.failures() == 2);
        l.failed();
        CHECK(l.link() == Link::absent);
        l.up();
        CHECK(l.link() == Link::answering && l.bringUps() == 2 && l.failures() == 0);
    }
    return Kvasir::Test::report();
}
