// Every / Deadline and their 32-bit forms, driven by a FakeClock. `now` is passed explicitly, so
// most cases are static_asserts on time points; the 32-bit forms are also run across the wrap.
#include "kvasir/Util/Periodic.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;
using Kvasir::Deadline;
using Kvasir::Deadline32;
using Kvasir::Every;
using Kvasir::Every32;

namespace {

using Ms   = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;
using Us   = Kvasir::Test::FakeClockT<std::chrono::microseconds>;
using MsTp = Ms::time_point;

constexpr MsTp at(std::int64_t ms) { return MsTp{std::chrono::milliseconds{ms}}; }

// ---- Every -------------------------------------------------------------------------------------

static_assert(
  [] {
      Every<Ms> e{10ms, at(100)};
      bool      ok = !e.due(at(99)) && e.due(at(100)) && !e.due(at(100)) && !e.due(at(109));
      ok           = ok && e.due(at(110)) && e.next() == at(120);
      return ok;
  }(),
  "due: not before first, once at first, then every period on the grid");

static_assert(
  [] {
      Every<Ms> e{10ms, at(0)};
      int       n = 0;
      while(e.due(at(35))) { ++n; }   // a stall of 3.5 periods: 0, 10, 20, 30
      return n == 4 && e.next() == at(40);
  }(),
  "due: catch-up burst of every missed period, grid kept");

static_assert(
  [] {
      Every<Ms> e{10ms, at(0)};
      bool      ok = e.dueCount(at(0)) == 1 && e.next() == at(10);   // on time
      ok           = ok && e.dueCount(at(9)) == 0;                   // early
      ok = ok && e.dueCount(at(19)) == 1 && e.next() == at(20);      // late within one period
      return ok;
  }(),
  "dueCount: 0 early, 1 on time and within a period");

// modm's periodic timer counts `if(diff != interval) count++` in a loop and returns 1 here; the
// division returns 2 (two periods passed: the one due at 10 and the one due at 20)
static_assert(
  [] {
      Every<Ms> e{10ms, at(10)};
      return e.dueCount(at(20)) == 2 && e.next() == at(30);
  }(),
  "dueCount: exactly two periods late is 2");

static_assert(
  [] {
      Every<Ms>  e{10ms, at(0)};
      auto const k = e.dueCount(at(47));   // due at 0, 10, 20, 30, 40
      return k == 5 && e.next() == at(50) && !e.due(at(49)) && e.due(at(50));
  }(),
  "dueCount: k after k periods, phase kept");

static_assert(
  [] {
      Every<Ms> e{10ms, at(0)};
      (void)e.due(at(0));
      e.skipTo(at(1000));
      return e.next() == at(1010) && !e.due(at(1009)) && e.due(at(1010));
  }(),
  "skipTo: rebase, no burst");

static_assert(
  [] {
      Every<Ms> e{10s, at(0)};
      (void)e.due(at(0));   // last grid point 0, next 10 s
      e.setPeriod(100ms);
      bool ok = e.next() == at(100) && e.due(at(100));
      e.setPeriod(1s);   // last grid point 100: next 1100, never earlier
      ok = ok && e.next() == at(1100) && !e.due(at(1099));
      return ok;
  }(),
  "setPeriod: counted from the last grid point");

// ---- Deadline ----------------------------------------------------------------------------------

static_assert(
  [] {
      Deadline<Ms> d;
      return d.stopped() && !d.expired(at(0)) && !d.armed(at(0)) && d.remaining(at(0)) == 0ms;
  }(),
  "Deadline: stopped by default");

static_assert(
  [] {
      Deadline<Ms> d;
      d.restart(50ms, at(100));
      bool ok = d.armed(at(149)) && !d.expired(at(149)) && d.remaining(at(120)) == 30ms;
      ok      = ok && d.expired(at(150)) && !d.armed(at(150)) && d.remaining(at(160)) == -10ms;
      ok      = ok && !d.due(at(149)) && d.due(at(150)) && d.stopped() && !d.due(at(151));
      return ok;
  }(),
  "Deadline: armed, expired, remaining signed, due once then stopped");

static_assert(
  [] {
      Deadline<Ms> d{50ms, at(0)};
      d.restart(50ms, at(40));   // moves the end
      bool ok = !d.expired(at(50)) && d.expired(at(90));
      d.stop();
      return ok && d.stopped() && !d.expired(at(1000));
  }(),
  "Deadline: restart while armed moves the end, stop");

static_assert(sizeof(Deadline<Ms>) == 8);
static_assert(sizeof(Every<Ms>) == 16);
static_assert(sizeof(Every32<Ms>) == 8 && sizeof(Deadline32<Ms>) == 8);

// ---- 32-bit forms across the wrap ----------------------------------------------------------------

constexpr std::int64_t Wrap = std::int64_t{1} << 32;

static_assert(
  [] {
      Every32<Ms> e{10ms, at(Wrap - 15)};
      bool        ok = e.due(at(Wrap - 15)) && !e.due(at(Wrap - 6));
      ok             = ok && e.due(at(Wrap - 5));                           // grid point Wrap - 5
      ok             = ok && !e.due(at(Wrap + 4)) && e.due(at(Wrap + 5));   // across the wrap
      ok             = ok && e.dueCount(at(Wrap + 36)) == 3;                // 15, 25, 35
      ok             = ok && !e.due(at(Wrap + 44)) && e.due(at(Wrap + 45));
      return ok;
  }(),
  "Every32: due and dueCount across 2^32 ms");

static_assert(
  [] {
      Every32<Ms> e{10ms, at(Wrap - 3)};
      (void)e.due(at(Wrap - 3));
      e.skipTo(at(Wrap + 100));
      return !e.due(at(Wrap + 109)) && e.due(at(Wrap + 110));
  }(),
  "Every32: skipTo across the wrap");

static_assert(
  [] {
      Deadline32<Ms> d;
      bool           ok = d.stopped() && !d.expired(at(0)) && d.remaining(at(0)).count() == 0;
      d.restart(20ms, at(Wrap - 10));
      ok = ok && d.armed(at(Wrap + 9)) && !d.expired(at(Wrap + 9));
      ok = ok && d.remaining(at(Wrap)).count() == 10;
      ok = ok && d.expired(at(Wrap + 10)) && d.remaining(at(Wrap + 12)).count() == -2;
      ok = ok && d.due(at(Wrap + 10)) && d.stopped();
      return ok;
  }(),
  "Deadline32 across the wrap");

// a clock whose now() returns a duration (AonTimer's shape) is not a PollClock
struct NotAClock {
    using duration   = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Ms, duration>;

    static duration now() { return {}; }
};

static_assert(!Kvasir::PollClock<NotAClock>);
static_assert(Kvasir::PollClock<Ms>);

}   // namespace

// the defaulted `now` reads the FakeClock: the run-time path
int main() {
    using Kvasir::Test::test;
    test("periodic with the clock");

    Ms::set(5s);
    Every<Ms> e{100ms};   // first due now
    CHECK(e.due());
    CHECK(!e.due());
    Ms::advance(250ms);
    CHECK(e.dueCount() == 2);
    CHECK(!e.due());

    Deadline<Ms> d{1s};
    Ms::advance(999ms);
    CHECK(d.armed());
    Ms::advance(1ms);
    CHECK(d.due());
    CHECK(d.stopped());

    // micro-second 32-bit form on a microsecond clock across 2^32 us (71.6 min)
    Us::set(std::chrono::microseconds{Wrap - 100});
    Every32<Us, std::micro> fast{1000us};
    CHECK(fast.due());
    Us::advance(999us);
    CHECK(!fast.due());
    Us::advance(1us);   // past the wrap now
    CHECK(fast.due());
    Us::advance(3000us);
    CHECK(fast.dueCount() == 3);

    return Kvasir::Test::report();
}
