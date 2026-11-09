// Kvasir::Control::Pid on an integral T (fixed point), and the duration conversions of both kinds:
// against a double Pid on the same scenarios (a stated tolerance per part), bit-exact against a
// 128-bit reference of the fixed-point arithmetic, at the range edges (in constant expressions, so
// an overflow would not compile), and both division paths. No clock: every dt is given.
#include "kvasir/Control/Pid.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ratio>

namespace {
using Kvasir::Control::Pid;
using Kvasir::Control::PidGains;
using namespace std::chrono_literals;
namespace Fx = Kvasir::Control::Detail::PidFixed;

// the first user's shape: 0.01 units in and out, gains in thousandths, tau and dt in milliseconds
using Fixed = Pid<std::int32_t, std::milli, std::chrono::milliseconds>;
using Real  = Pid<double>;
using Ticks = std::chrono::duration<std::int64_t, std::ratio<1, 48'000'000>>;   // a 48 MHz SysTick

constexpr std::int32_t i32max = std::numeric_limits<std::int32_t>::max();
constexpr std::int32_t i32min = std::numeric_limits<std::int32_t>::min();

constexpr Fixed::Gains gains{.kp     = 2000,
                             .ki     = 1000,
                             .kd     = 500,
                             .tau    = 100,
                             .outMin = 0,
                             .outMax = 10000};

// the same controller in double: gains / 1000, tau and dt in seconds, values in the same counts
constexpr Real::Gains realOf(Fixed::Gains g) {
    return {.kp     = g.kp / 1000.0,
            .ki     = g.ki / 1000.0,
            .kd     = g.kd / 1000.0,
            .tau    = g.tau / 1000.0,
            .outMin = static_cast<double>(g.outMin),
            .outMax = static_cast<double>(g.outMax)};
}

static_assert(std::is_same_v<decltype(Pid{gains}),
                             Fixed>,
              "the gains name the Pid (CTAD)");
static_assert(std::is_same_v<Fixed::Gains,
                             PidGains<std::int32_t,
                                      std::milli,
                                      std::chrono::milliseconds>>);
static_assert(std::is_same_v<Pid<float>,
                             Pid<float,
                                 std::ratio<1>,
                                 std::chrono::duration<float>>>,
              "a float defaults to plain gains and seconds");

// ---- the algorithm, as for float ----

static_assert(
  [] {
      Fixed      p{gains};
      auto const u = p.update(5000, 4000, 1000);   // P only, dt ignored
      return u == 2000 && p.proportional() == 2000 && p.integral() == 0 && p.derivative() == 0;
  }(),
  "the first update is P only");

static_assert(
  [] {
      Fixed p{gains};
      for(int i = 0; i < 100; ++i) {
          auto const u = p.update(100'000, 0, 100);
          if(u < 0 || u > 10000) { return false; }
      }
      return true;
  }(),
  "the output never leaves the limits");

static_assert(
  [] {
      Fixed p{gains};
      for(int i = 0; i < 100; ++i) { (void)p.update(100'000, 0, 100); }
      return p.integral() == 0;   // P = 200000, far past outMax: no room
  }(),
  "anti-windup at the upper limit");

static_assert(
  [] {
      Fixed p{gains};
      for(int i = 0; i < 100; ++i) { (void)p.update(0, 100'000, 100); }
      return p.integral() == 0 && p.output() == 0;   // P = -200000, far below outMin
  }(),
  "anti-windup at the lower limit");

static_assert(
  [] {
      // kp 0.2: P = 600 for an error of 3000; the integral fills exactly the 400 left to 1000
      Fixed p{
        {.kp = 200, .ki = 1000, .kd = 0, .tau = 100, .outMin = 0, .outMax = 1000}
      };
      for(int i = 0; i < 100; ++i) { (void)p.update(3000, 0, 100); }
      return p.proportional() == 600 && p.integral() == 400 && p.output() == 1000;
  }(),
  "the integral gets exactly the room P leaves");

static_assert(
  [] {
      Fixed p{gains};
      (void)p.update(1000, 1000, 10);
      (void)p.update(1000, 1000, 10);
      (void)p.update(9000, 1000, 10);   // setpoint step, measurement constant
      return p.derivative() == 0;
  }(),
  "a setpoint step does not kick the derivative");

static_assert(
  [] {
      Fixed p{gains};
      (void)p.update(5000, 1000, 10);
      (void)p.update(5000, 2000, 10);
      auto const i = p.integral();
      auto const d = p.derivative();
      (void)p.update(5000, 3000, 0);
      (void)p.update(5000, 4000, -10);
      (void)p.update(5000, 4500, 0ms);
      return i != 0 && d != 0 && p.integral() == i && p.derivative() == d;
  }(),
  "dt <= 0 holds I and D");

static_assert(
  [] {
      Fixed p{gains};
      (void)p.update(5000, 1000, 10);
      (void)p.update(5000, 2000, 10);
      p.reset();
      auto const u = p.update(5000, 4000, 10);
      return u == 2000 && p.integral() == 0 && p.derivative() == 0;
  }(),
  "reset makes the next update P only again");

static_assert(
  [] {
      Fixed p{
        {.kp = 200, .ki = 1000, .kd = 0, .tau = 100, .outMin = 0, .outMax = 1000}
      };
      for(int i = 0; i < 100; ++i) { (void)p.update(3000, 0, 100); }
      bool const full = p.integral() == 400;
      p.setGains({.kp = 200, .ki = 1000, .kd = 0, .tau = 100, .outMin = 0, .outMax = 300});
      return full && p.integral() == 300 && p.gains().outMax == 300;
  }(),
  "setGains clamps the integral into the new limits");

static_assert(
  [] {
      // kp 1.0, ki 1.0 /s: an error of 1000 for 1 s in 10 ms steps integrates to exactly 1000
      Fixed p{
        {.kp = 1000, .ki = 1000, .kd = 0, .tau = 0, .outMin = -100'000, .outMax = 100'000}
      };
      for(int i = 0; i <= 100; ++i) { (void)p.update(1000, 0, 10); }
      return p.integral() == 1000;
  }(),
  "the integral accumulates exactly (100 steps of 10 ms, no drift)");

// ---- rounding: to nearest, ties away from zero ----

constexpr std::int32_t proportionalFor(std::int32_t kp,
                                       std::int32_t error) {
    Fixed p{
      {.kp = kp, .ki = 0, .kd = 0, .tau = 0, .outMin = i32min, .outMax = i32max}
    };
    return p.update(error, 0, 10);
}

static_assert(proportionalFor(1,
                              499)
              == 0);
static_assert(proportionalFor(1,
                              500)
                == 1,
              "0.5 rounds away from zero");
static_assert(proportionalFor(1,
                              -500)
                == -1,
              "-0.5 rounds away from zero");
static_assert(proportionalFor(1,
                              1500)
              == 2);
static_assert(proportionalFor(1,
                              2500)
                == 3,
              "not to even");
static_assert(proportionalFor(1,
                              -2500)
              == -3);
static_assert(proportionalFor(1,
                              -2499)
              == -2);
static_assert(proportionalFor(333,
                              3)
                == 1,
              "0.999 -> 1");
static_assert(proportionalFor(-300,
                              1000)
              == -300);

constexpr std::int32_t countsFor(auto dt) {
    // kp 0, ki 1.0 /s on an error of 1'000'000: the integral after one step is the dt in ms
    Fixed p{
      {.kp = 0, .ki = 1000, .kd = 0, .tau = 0, .outMin = i32min, .outMax = i32max}
    };
    (void)p.update(1'000'000, 0, dt);   // primes
    (void)p.update(1'000'000, 0, dt);
    return p.integral() / 1000;
}

static_assert(countsFor(Ticks{480'000}) == 10,
              "48 MHz ticks to ms");
static_assert(countsFor(Ticks{23'999}) == 0,
              "0.49998 ms rounds to 0 (holds I and D)");
static_assert(countsFor(Ticks{24'000}) == 1,
              "0.5 ms rounds away from zero");
static_assert(countsFor(Ticks{71'999}) == 1);
static_assert(countsFor(Ticks{72'000}) == 2,
              "1.5 ms -> 2");
static_assert(countsFor(10ms) == 10 && countsFor(2s) == 2000 && countsFor(1500us) == 2);
static_assert(countsFor(std::chrono::duration<double,
                                              std::milli>{10.5})
                == 11,
              "a floating count rounds too");
static_assert(countsFor(std::chrono::duration<float>{0.25f}) == 250);

// ---- range edges: in constant expressions, so an int overflow would be a compile error ----

using Plain = Pid<std::int32_t, std::ratio<1>, std::chrono::milliseconds>;
constexpr Plain::Gains wide{.kp     = i32max,
                            .ki     = i32max,
                            .kd     = i32max,
                            .tau    = 0,
                            .outMin = i32min,
                            .outMax = i32max};

static_assert(
  [] {
      Plain p{wide};
      bool  ok = p.update(i32max, i32min, i32max) == i32max && p.proportional() == i32max;
      ok       = ok && p.update(i32min, i32max, i32max) == i32min && p.proportional() == i32min;
      return ok;
  }(),
  "P saturates at T's range, the error first");

static_assert(
  [] {
      Plain p{wide};
      bool  integralAtLimit = false;
      bool  derivativeAtMin = false;
      for(int i = 0; i < 20; ++i) {
          bool const up = i % 2 == 0;
          (void)p.update(up ? i32max : i32min, up ? i32min : i32max, i % 3 == 0 ? 1 : i32max);
          integralAtLimit = integralAtLimit || p.integral() == i32min;
          derivativeAtMin = derivativeAtMin || p.derivative() == i32min;
      }
      (void)p.update(0, i32min, 1);   // the largest step in one count of time
      return integralAtLimit && derivativeAtMin && p.derivative() == i32max && p.output() == i32max;
  }(),
  "I and D saturate, no overflow, at every T value of gains, values and dt");

static_assert(
  [] {
      Pid<std::int32_t, std::milli, std::chrono::microseconds> p{
        {.kp     = i32max,
         .ki     = i32max,
         .kd     = i32max,
         .tau    = i32max,
         .outMin = i32min,
         .outMax = i32max}
      };
      for(int i = 0; i < 20; ++i) {
          (void)p.update(i % 3 == 0 ? i32max : i32min, i % 2 == 0 ? i32min : i32max, i32max);
      }
      return p.output() >= i32min;
  }(),
  "another unit pair: the same, no overflow");

static_assert(
  [] {
      Fixed p{gains};
      (void)p.update(0, 0, 1);
      // a duration far past T's range: dt saturates at T's maximum (the integral fills the 8000
      // P = 2000 leaves); one at the negative end holds I and D
      (void)p.update(1000, 0, Ticks{std::numeric_limits<std::int64_t>::max()});
      bool const full = p.integral() == 8000 && p.output() == 10000 && p.wideDivisions() != 0;
      (void)p.update(1000, 0, Ticks{std::numeric_limits<std::int64_t>::min()});
      return full && p.integral() == 8000 && p.output() == 2000;
  }(),
  "a duration at Rep's limits converts without overflow");

static_assert(
  [] {
      Pid<std::int16_t, std::milli, std::chrono::milliseconds> p{
        {.kp = 32767, .ki = 0, .kd = 0, .tau = 0, .outMin = -32768, .outMax = 32767}
      };
      return p.update(32767, -32768, 10) == 32767 && p.update(-32768, 32767, 10) == -32768
          && p.update(1000, 0, 10) == 32767;   // 32.767 * 1000 saturates at int16
  }(),
  "int16_t: the same algorithm, saturating at its range");

static_assert(
  [] {
      Fixed p{
        {.kp = 0, .ki = 0, .kd = 1000, .tau = -50, .outMin = i32min, .outMax = i32max}
      };
      (void)p.update(0, 0, 10);
      (void)p.update(0, 100, 10);        // a negative tau counts as 0: D = -2 kd step / dt
      return p.derivative() == -20000;   // kd 1 s = 1000 ms: -2 * 1000 * 100 / 10
  }(),
  "a negative tau counts as 0 (never a division by zero)");

// ---- the division and multiply paths of the arithmetic ----

constexpr std::int64_t two32 = std::int64_t{1} << 32;

struct Divided {
    std::int64_t value;
    int          wide;
};

constexpr Divided divided(std::int64_t  n,
                          std::uint64_t d) {
    std::uint8_t wide{};
    auto const   v = Fx::divide(n, d, 0, wide);
    return {v, wide};
}

template<std::intmax_t D>
constexpr Divided dividedBy(std::int64_t n) {
    std::uint8_t wide{};
    auto const   v = Fx::divide<D>(n, wide);
    return {v, wide};
}

// a runtime divisor: 32 bits while |n| + d / 2 < 2^32
static_assert(divided(two32 - 1 - 50,
                      100)
                  .value
                == (two32 - 1) / 100
              && divided(two32 - 1 - 50,
                         100)
                     .wide
                   == 0);
static_assert(divided(two32 - 50,
                      100)
                  .value
                == two32 / 100
              && divided(two32 - 50,
                         100)
                     .wide
                   == 1);
static_assert(divided(-(two32 - 51),
                      100)
                  .value
                == -((two32 - 1) / 100)
              && divided(-(two32 - 51),
                         100)
                     .wide
                   == 0);
static_assert(divided(-(two32 - 50),
                      100)
                .wide
              == 1);
static_assert(divided(7,
                      two32)
                    .value
                  == 0
                && divided(7,
                           two32)
                       .wide
                     == 1,
              "a divisor of 2^32 is 64-bit");
static_assert(divided(150,
                      100)
                  .value
                == 2
              && divided(149,
                         100)
                     .value
                   == 1
              && divided(-150,
                         100)
                     .value
                   == -2);
static_assert(divided(5,
                      3)
                  .value
                == 2
              && divided(4,
                         3)
                     .value
                   == 1
              && divided(-5,
                         3)
                     .value
                   == -2);
// a compile-time 1000 = 8 * 125: (|n| + 500) >> 3, then a 32-bit division by 125 while that fits
constexpr std::int64_t lastNarrow = (two32 << 3) - 1 - 500;
static_assert(dividedBy<1000>(lastNarrow).wide == 0
              && dividedBy<1000>(lastNarrow).value == (lastNarrow + 500) / 1000);
static_assert(dividedBy<1000>(lastNarrow + 1).wide == 1
              && dividedBy<1000>(lastNarrow + 1).value == (lastNarrow + 501) / 1000);
static_assert(dividedBy<1000>(-lastNarrow).value == -((lastNarrow + 500) / 1000));
static_assert(dividedBy<256>(std::int64_t{1} << 60).wide == 0,
              "a power of two is a shift only");
static_assert(dividedBy<256>(128).value == 1 && dividedBy<256>(127).value == 0
              && dividedBy<256>(-128).value == -1);
static_assert(dividedBy<2'000'000>(3'000'000).value == 2
              && dividedBy<2'000'000>(2'999'999).value == 1);
static_assert(dividedBy<48'000>(480'000).value == 10 && dividedBy<48'000>(24'000).value == 1);

// the 64-bit division without a library routine: 16-bit digits below a divisor of 2^16, bits above
static_assert(Fx::divideLong(0xFFFF'FFFF'FFFF'FFFFU,
                             65535)
              == 0xFFFF'FFFF'FFFF'FFFFU / 65535);
static_assert(Fx::divideLong(0xFFFF'FFFF'FFFF'FFFFU,
                             65536)
              == 0xFFFF'FFFF'FFFF'FFFFU / 65536);
static_assert(Fx::divideLong(0x1234'5678'9ABC'DEF0U,
                             125)
              == 0x1234'5678'9ABC'DEF0U / 125);
static_assert(Fx::divideLong(0x1234'5678'9ABC'DEF0U,
                             3'000'000'017U)
              == 0x1234'5678'9ABC'DEF0U / 3'000'000'017U);
static_assert(Fx::divideLong(std::uint64_t{1} << 62,
                             (std::uint64_t{1} << 61) + 1)
              == 1);
static_assert(Fx::divideLong(7,
                             65537)
                == 0
              && Fx::divideLong(65537,
                                65537)
                   == 1);

// mul: exact below the limit 2^61, saturated from there, in every path
static_assert(Fx::mul(65535,
                      i32max)
                == std::int64_t{65535} * i32max,
              "16 x 32 bits");
static_assert(Fx::mul(-65535,
                      two32 - 1)
              == -std::int64_t{65535} * (two32 - 1));
static_assert(Fx::mul(65536,
                      two32 - 1)
                == std::int64_t{65536} * (two32 - 1),
              "32 x 32 bits");
static_assert(Fx::mul(i32min,
                      i32min)
                == Fx::limit,
              "2^62: saturated");
static_assert(Fx::mul(i32min,
                      -(std::int64_t{1} << 30))
                == Fx::limit,
              "2^61 itself is exact");
static_assert(Fx::mul(i32min,
                      (std::int64_t{1} << 30) - 1)
              == std::int64_t{i32min} * ((std::int64_t{1} << 30) - 1));
static_assert(Fx::mul(5,
                      (std::int64_t{1} << 59))
                == Fx::limit,
              "the split path, saturated");
static_assert(Fx::mul(-4,
                      (std::int64_t{1} << 59))
                == -Fx::limit,
              "the split path at 2^61: exact");
static_assert(Fx::mul(3,
                      (std::int64_t{1} << 59))
              == 3 * (std::int64_t{1} << 59));
static_assert(Fx::mul(3,
                      (std::int64_t{1} << 58) + 5)
                == 3 * ((std::int64_t{1} << 58) + 5),
              "the split path, exact");
static_assert(Fx::mul(-(two32 + 1),
                      two32 + 1)
                == -Fx::limit,
              "both >= 2^32");
static_assert(Fx::mul(0,
                      Fx::limit)
                == 0
              && Fx::mul(Fx::limit,
                         1)
                   == Fx::limit);

// ---- a 128-bit reference of the fixed-point algorithm (milli, ms): no 32-bit paths, no
// split multiplies, no saturation of the intermediates - for inputs inside the exact range ----

__extension__ using I128 = __int128;

struct Reference {
    Fixed::Gains g;
    std::int32_t p{}, i{}, d{}, out{}, prevE{}, prevM{};
    I128         acc{}, dq{};
    bool         primed{};
    bool         exact{true};   // every intermediate within +-2^61, the Pid's exact range

    constexpr I128 checked(I128 v) {
        I128 const lim = I128{1} << 61;
        if(v > lim || v < -lim) { exact = false; }
        return v;
    }

    static constexpr I128 roundDiv(I128 n,
                                   I128 den) {
        I128 const q = ((n < 0 ? -n : n) + den / 2) / den;
        return n < 0 ? -q : q;
    }

    static constexpr std::int32_t sat(I128 v) {
        return static_cast<std::int32_t>(std::clamp(v, I128{i32min}, I128{i32max}));
    }

    constexpr std::int32_t update(std::int32_t sp,
                                  std::int32_t m,
                                  std::int32_t dt) {
        std::int32_t const e = sat(I128{sp} - m);
        p                    = sat(roundDiv(checked(I128{g.kp} * e), 1000));
        I128 sum             = p;
        if(primed && dt > 0) {
            acc += checked(I128{g.ki} * dt * (I128{e} + prevE));
            I128 const iMax = g.outMax > p ? I128{g.outMax} - p : 0;
            I128 const iMin = g.outMin < p ? I128{g.outMin} - p : 0;
            acc             = std::clamp(acc, checked(iMin * 2'000'000), checked(iMax * 2'000'000));
            // the Pid keeps the integral's whole counts in 32 bits
            if(acc / 2'000'000 > i32max || acc / 2'000'000 < i32min) { exact = false; }
            i              = sat(roundDiv(acc, 2'000'000));
            I128 const tau = g.tau > 0 ? g.tau : 0;
            dq -= roundDiv(checked(512 * I128{g.kd} * (I128{m} - prevM))
                             + checked(2 * I128{dt} * dq),
                           2 * tau + dt);
            (void)checked(dq);
            d = sat(roundDiv(dq, 256));
            sum += I128{i} + d;
        }
        primed     = true;
        prevM      = m;
        prevE      = e;
        return out = sat(std::clamp(sum, I128{g.outMin}, I128{g.outMax}));
    }
};

// a deterministic generator (xorshift64)
struct Random {
    std::uint64_t s{0x9E37'79B9'7F4A'7C15U};

    constexpr std::uint64_t next() {
        s ^= s << 13U;
        s ^= s >> 7U;
        s ^= s << 17U;
        return s;
    }

    /// in [-2^bits, 2^bits)
    constexpr std::int64_t signedBits(unsigned bits) {
        return static_cast<std::int64_t>(next() >> (64U - bits - 1U)) - (std::int64_t{1} << bits);
    }

    constexpr std::int32_t upTo(std::int32_t max) {
        return static_cast<std::int32_t>(next() % static_cast<std::uint64_t>(max + 1));
    }
};

// bit-exact against the reference, small and large magnitudes mixed; counts the wide divisions
struct ExactRun {
    int  mismatches{};
    int  updates{};         // compared: inside the exact range
    int  narrowUpdates{};   // no 64-bit division
    long wideDivisions{};
};

constexpr ExactRun exactRun(int steps) {
    ExactRun r{};
    Random   rnd{};
    for(int run = 0; run < steps / 200; ++run) {
        bool const big = run % 3 == 2;   // every third run far out, the others in the user's range
        Fixed::Gains g{.kp     = big ? rnd.upTo(1 << 9) : rnd.upTo(5000),
                       .ki     = big ? rnd.upTo(1 << 12) : rnd.upTo(1000),
                       .kd     = big ? rnd.upTo(1 << 16) : rnd.upTo(5000),
                       .tau    = big ? rnd.upTo(1 << 20) : 10 + rnd.upTo(4990),
                       .outMin = big ? -rnd.upTo(1 << 29) : 0,
                       .outMax = big ? rnd.upTo(1 << 29) : 1000};
        Fixed        pid{g};
        Reference    ref{g};
        std::int32_t m = 0;
        for(int i = 0; i < 200; ++i) {
            auto const sp = big ? static_cast<std::int32_t>(rnd.signedBits(30)) : rnd.upTo(80000);
            m = big ? static_cast<std::int32_t>(rnd.signedBits(30))
                    : std::clamp(m + static_cast<std::int32_t>(rnd.signedBits(9)), 0, 80000);
            auto const dt = static_cast<std::int32_t>(
              i % 17 == 16 ? -rnd.upTo(5) : (big ? rnd.upTo(1 << 16) : 10 + rnd.upTo(90)));
            if(i == 100 && run % 2 == 1) {
                pid.reset();
                ref = Reference{g};
            }
            auto const a = pid.update(sp, m, dt);
            auto const b = ref.update(sp, m, dt);
            if(!ref.exact) { break; }   // past the documented range: saturation, not compared
            ++r.updates;
            if(a != b || pid.proportional() != ref.p || pid.integral() != ref.i
               || pid.derivative() != ref.d)
            {
                ++r.mismatches;
            }
            if(pid.wideDivisions() == 0) { ++r.narrowUpdates; }
            r.wideDivisions += pid.wideDivisions();
        }
    }
    return r;
}

// the closed loop at the first user's ranges: no update takes a 64-bit division
struct UserRun {
    int          wideUpdates{};
    int          updates{};
    std::int32_t finalCenti{};
};

UserRun userRanges(std::int32_t kp,
                   std::int32_t ki,
                   std::int32_t kd,
                   std::int32_t tau,
                   Ticks        period) {
    Fixed pid{
      {.kp = kp, .ki = ki, .kd = kd, .tau = tau, .outMin = 0, .outMax = 1000}
    };
    UserRun r{};
    // a heater from 20 C: x' = (0.6 C per output count u - (x - 20 C)) / 4 s, in 0.01 C - full
    // power heats at 150 C/s at first (a retailer's "350 C in 2 s" is 165)
    double     x  = 2000.0;
    auto const dt = std::chrono::duration<double>(period).count();
    for(int i = 0; i < 3000; ++i) {
        std::int32_t const set = i < 1500 ? 50000 : 30000;   // 500 C, then down to 300 C
        auto const         m   = static_cast<std::int32_t>(std::lround(x));
        auto const         u   = pid.update(set, m, period);
        x += (60.0 * u - (x - 2000.0)) / 4.0 * dt;
        ++r.updates;
        if(pid.wideDivisions() != 0) { ++r.wideUpdates; }
    }
    r.finalCenti = static_cast<std::int32_t>(x);
    return r;
}

// ---- the integer Pid against the double one, in open loop: both see the same integer
// measurements, so the difference is the fixed point's alone. Per part: P within 0.5, I within 0.5
// (1.0 after an anti-windup clamp: the int clamps to outMax - P with P rounded), D within
// 0.5 + (2 tau + dt) / (1024 dt) (its 8 fraction bits), the output within their sum ----

struct Step {
    std::int32_t setpoint;
    std::int32_t measurement;
    std::int32_t dtMs;
};

struct Worst {
    double p{}, i{}, d{}, out{};
    int    atMax{}, atMin{};
};

template<std::size_t N>
Worst compare(Fixed::Gains         g,
              std::array<Step,
                         N> const& steps,
              int                  resetAt  = -1,
              int                  gainsAt  = -1,
              Fixed::Gains         newGains = {}) {
    Fixed pid{g};
    Real  real{realOf(g)};
    Worst w{};
    for(std::size_t k = 0; k < N; ++k) {
        if(static_cast<int>(k) == resetAt) {
            pid.reset();
            real.reset();
        }
        if(static_cast<int>(k) == gainsAt) {
            pid.setGains(newGains);
            real.setGains(realOf(newGains));
        }
        auto const& s = steps[k];
        auto const  a = pid.update(s.setpoint, s.measurement, s.dtMs);
        auto const  b = real.update(s.setpoint, s.measurement, s.dtMs / 1000.0);
        w.p           = std::max(w.p, std::abs(pid.proportional() - real.proportional()));
        w.i           = std::max(w.i, std::abs(pid.integral() - real.integral()));
        w.d           = std::max(w.d, std::abs(pid.derivative() - real.derivative()));
        w.out         = std::max(w.out, std::abs(a - b));
        if(a == pid.gains().outMax) { ++w.atMax; }
        if(a == pid.gains().outMin) { ++w.atMin; }
    }
    return w;
}

constexpr double eps = 1e-9;

void checkWithin(Worst        w,
                 double       iBound,
                 Fixed::Gains g,
                 double       dtMs) {
    double const dBound = 0.5 + (2.0 * g.tau + dtMs) / (1024.0 * dtMs);
    CHECK(w.p <= 0.5 + eps);
    CHECK(w.i <= iBound + eps);
    CHECK(w.d <= dBound + eps);
    CHECK(w.out <= 0.5 + iBound + dBound + eps);
}

/// the measurement of a first-order plant (gain 4, lag 2 s) driven by the double Pid, in counts
template<std::size_t N>
std::array<Step,
           N>
plantRun(Fixed::Gains g,
         std::int32_t setpoint,
         std::int32_t dtMs) {
    Real                real{realOf(g)};
    std::array<Step, N> steps{};
    double              x = 0.0;
    for(std::size_t k = 0; k < N; ++k) {
        auto const m = static_cast<std::int32_t>(std::lround(x));
        steps[k]     = {setpoint, m, dtMs};
        auto const u = real.update(setpoint, m, dtMs / 1000.0);
        x += (4.0 * u - x) * (dtMs / 1000.0) / 2.0;
    }
    return steps;
}

}   // namespace

int main() {
    Kvasir::Test::test("bit-exact against the 128-bit reference");
    {
        auto const r = exactRun(60'000);
        CHECK(r.mismatches == 0);
        CHECK(r.updates > 50'000);
        CHECK(r.narrowUpdates > 30'000);   // the user's ranges: 32-bit only
        CHECK(r.wideDivisions > 1000);     // the far-out runs: the 64-bit paths, as exact
        std::print("exact: {} updates, {} without a 64-bit division, {} 64-bit divisions\n",
                   r.updates,
                   r.narrowUpdates,
                   r.wideDivisions);
    }

    Kvasir::Test::test("the first user's ranges stay in 32 bits");
    {
        // every half-cycle (10 ms) or every tenth (100 ms), with a tick of jitter; tau from dt / 2
        // (below, the Tustin filter rings: see the next block) to 5 s, the largest gains
        for(auto const period :
            {Ticks{480'000}, Ticks{4'800'000}, Ticks{479'000}, Ticks{4'810'000}})
        {
            auto const halfDt = static_cast<std::int32_t>(period / 1ms / 2);
            for(std::int32_t const tau : {std::max(10, halfDt), 700, 5000}) {
                auto const r = userRanges(5000, 1000, 5000, tau, period);
                CHECK(r.updates == 3000);
                CHECK(r.wideUpdates == 0);
                auto const s = userRanges(300, 0, 200, tau, period);
                CHECK(s.wideUpdates == 0);
            }
        }
        // tau = 10 ms at dt = 100 ms: (2 tau - dt) < 0, D rings to +-200000 counts and its update
        // takes the 64-bit division - as exact (the reference block above covers that path)
        CHECK(userRanges(5000, 1000, 5000, 10, Ticks{4'800'000}).wideUpdates > 1000);
        auto const r = userRanges(1000, 200, 200, 700, Ticks{480'000});
        CHECK(std::abs(r.finalCenti - 30000) < 100);   // it regulates (PID at 10 ms)
        // a measurement jump of 800 C in one step at kd 5: one 64-bit division, as exact
        Fixed pid{
          {.kp = 5000, .ki = 1000, .kd = 5000, .tau = 700, .outMin = 0, .outMax = 1000}
        };
        Reference ref{pid.gains()};
        (void)pid.update(30000, 30000, 10);
        (void)ref.update(30000, 30000, 10);
        CHECK(pid.update(30000, 30000 - 80000, 10) == ref.update(30000, 30000 - 80000, 10));
        CHECK(pid.wideDivisions() >= 1);
        CHECK(pid.derivative() == ref.d);
    }

    Kvasir::Test::test("against the double Pid: plant step response");
    {
        constexpr Fixed::Gains g{.kp     = 1000,
                                 .ki     = 800,
                                 .kd     = 200,
                                 .tau    = 50,
                                 .outMin = 0,
                                 .outMax = 10000};
        auto const             steps = plantRun<4000>(g, 20000, 10);
        checkWithin(compare(g, steps), 0.5, g, 10.0);
        // the end of the step response settles at the setpoint
        CHECK(std::abs(steps.back().measurement - 20000) <= 20);
    }

    Kvasir::Test::test("against the double Pid: anti-windup at both limits");
    {
        constexpr Fixed::Gains g{.kp     = 333,
                                 .ki     = 777,
                                 .kd     = 111,
                                 .tau    = 70,
                                 .outMin = -500,
                                 .outMax = 1000};
        std::array<Step, 900>  steps{};
        for(std::size_t k = 0; k < steps.size(); ++k) {
            auto const         phase = k / 150;   // far above, near, far below, near, ...
            std::int32_t const sp    = phase % 4 == 0 ? 90000 : phase % 4 == 2 ? -90000 : 1234;
            steps[k]                 = {sp, static_cast<std::int32_t>((k * 37) % 401) - 200, 10};
        }
        auto const w = compare(g, steps);
        checkWithin(w, 1.0, g, 10.0);
        CHECK(w.atMax > 100 && w.atMin > 100);   // both limits held the output
    }

    Kvasir::Test::test("against the double Pid: a measurement jump through the D filter");
    {
        constexpr Fixed::Gains g{.kp     = 500,
                                 .ki     = 0,
                                 .kd     = 2000,
                                 .tau    = 700,
                                 .outMin = -100'000,
                                 .outMax = 100'000};
        std::array<Step, 600>  steps{};
        for(std::size_t k = 0; k < steps.size(); ++k) {
            std::int32_t const m = k < 100 ? 20000
                                 : k < 300 ? 25000
                                           : 25000 - static_cast<std::int32_t>(k - 300) * 7;
            steps[k]             = {25000, m, 10};
        }
        checkWithin(compare(g, steps), 0.5, g, 10.0);
        // the kick itself: -2 kd step / (2 tau + dt) = -2 * 2 * 5000 / 1.41 s
        Fixed pid{g};
        (void)pid.update(25000, 20000, 10);
        (void)pid.update(25000, 25000, 10);
        CHECK(std::abs(pid.derivative() - (-2.0 * 2.0 * 5000.0 / 1.41)) <= 0.5 + eps);
    }

    Kvasir::Test::test("against the double Pid: dt <= 0, reset and setGains on the way");
    {
        constexpr Fixed::Gains g{.kp     = 1500,
                                 .ki     = 400,
                                 .kd     = 300,
                                 .tau    = 100,
                                 .outMin = 0,
                                 .outMax = 5000};
        constexpr Fixed::Gains h{.kp     = 700,
                                 .ki     = 900,
                                 .kd     = 50,
                                 .tau    = 30,
                                 .outMin = 100,
                                 .outMax = 2000};
        auto                   steps = plantRun<1500>(g, 8000, 20);
        for(std::size_t k = 0; k < steps.size(); k += 13) {
            steps[k].dtMs = (k % 2 == 0) ? 0 : -20;
        }
        checkWithin(compare(g, steps, 400, 900, h), 1.0, g, 20.0);
        checkWithin(compare(h, steps, 700), 1.0, h, 20.0);
    }

    Kvasir::Test::test("closed loop: the integer Pid drives the plant as the double one does");
    {
        constexpr Fixed::Gains g{.kp     = 1000,
                                 .ki     = 800,
                                 .kd     = 200,
                                 .tau    = 50,
                                 .outMin = 0,
                                 .outMax = 10000};
        Fixed                  pid{g};
        Real                   real{realOf(g)};
        double                 x     = 0.0;
        double                 y     = 0.0;
        double                 worst = 0.0;
        for(int k = 0; k < 4000; ++k) {
            auto const u = pid.update(20000, static_cast<std::int32_t>(std::lround(x)), 10ms);
            auto const v = real.update(20000, static_cast<double>(std::lround(y)), 10ms);
            x += (4.0 * u - x) * 0.01 / 2.0;
            y += (4.0 * v - y) * 0.01 / 2.0;
            worst = std::max(worst, std::abs(x - y));
        }
        CHECK(worst < 20.0);   // 0.1 % of the setpoint
        CHECK(std::abs(x - 20000.0) < 5.0 && std::abs(y - 20000.0) < 5.0);
        std::print("closed loop: worst |int - double| {:.2f} counts, ends {:.2f} / {:.2f}\n",
                   worst,
                   x,
                   y);
    }

    Kvasir::Test::test("float: other units and the duration conversion");
    {
        // the same controller in milli-gains and milliseconds computes the same within float rounding
        Pid<float> p{
          {.kp = 2.0f, .ki = 1.0f, .kd = 0.5f, .tau = 0.1f, .outMin = 0.0f, .outMax = 1e4f}
        };
        Pid<float, std::milli, std::chrono::milliseconds> q{
          {.kp     = 2000.0f,
           .ki     = 1000.0f,
           .kd     = 500.0f,
           .tau    = 100.0f,
           .outMin = 0.0f,
           .outMax = 1e4f}
        };
        float worst = 0.0f;
        for(int k = 0; k < 500; ++k) {
            float const m = static_cast<float>((k * 53) % 300);
            worst
              = std::max(worst, std::abs(p.update(500.0f, m, 10ms) - q.update(500.0f, m, 10ms)));
        }
        CHECK(worst < 1e-2f);
        // the duration is count * (num / den) as one float multiply - no division
        float const volatile sp = 5.0f;
        Pid<float> a{
          {.kp = 2.0f, .ki = 1.0f, .kd = 0.5f, .tau = 0.1f, .outMin = 0.0f, .outMax = 10.0f}
        };
        Pid<float>      b{a.gains()};
        constexpr float tick = static_cast<float>(1.0L / 48'000'000.0L);
        for(std::int64_t const n : {std::int64_t{480'000}, std::int64_t{1} << 40}) {
            a.reset();
            b.reset();
            (void)a.update(sp, 1.0f, Ticks{n});
            (void)b.update(sp, 1.0f, static_cast<float>(n) * tick);
            CHECK(a.update(sp, 2.0f, Ticks{n}) == b.update(sp, 2.0f, static_cast<float>(n) * tick));
            CHECK(a.integral() == b.integral() && a.derivative() == b.derivative());
        }
        CHECK(a.wideDivisions() == 0);
    }

    return Kvasir::Test::report();
}
