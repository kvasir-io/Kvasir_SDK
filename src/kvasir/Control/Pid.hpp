#pragma once
// A PID controller with output limits, anti-windup and a filtered derivative, in the band-limited
// form (trapezoid integral, derivative on the measurement through a first-order filter, Tustin).
// One implementation for two kinds of value type T: floating point, or a signed integer of at most
// 32 bits as fixed point.
//
//     inline constexpr Kvasir::Control::PidGains<float> Gains{
//       .kp = 0.3f, .ki = 0.0f, .kd = 0.2f, .tau = 0.7f, .outMin = 0.0f, .outMax = 10.0f};
//     Kvasir::Control::Pid<float> pid{Gains};
//     float const u = pid.update(setpoint, measured, now - lastAt);   // dt in seconds or a duration
//
// No clock inside: dt comes from the caller, so the controller runs in an ISR or a test alike. The
// first update after construction or reset() is P only and ignores dt; dt <= 0 holds I and D. A
// positive kd damps (the derivative is taken on the measurement, so a setpoint step does not kick
// the output). Per update, with e = setpoint - measurement and m the measurement:
//
//     P  = kp e
//     I += ki dt (e + e_prev) / 2, then clamped to the room P leaves inside [outMin, outMax]
//     D  = (-2 kd (m - m_prev) + (2 tau - dt) D) / (2 tau + dt)
//     output = clamp(P + I + D, outMin, outMax)
//
// Keep tau >= dt / 2: below, the filter's pole (2 tau - dt) / (2 tau + dt) is negative and D rings.
//
// Units: Pid<T, GainUnit, TimeUnit>. GainUnit (a std::ratio) is the value of one count of kp, ki
// and kd; TimeUnit (a std::chrono::duration type, only its period counts) is the length of one count
// of tau and of a plain-number dt. The gains mean the same whatever TimeUnit is: kp is output per
// input, ki output per input and second, kd output seconds per input. A floating T defaults to
// std::ratio<1> and seconds, Pid<float> is the plain form above; an integral T states both.
//
// Fixed point (integral T). Worked example - a heater, setpoint and measurement in 0.01 C, output
// in 0.01 half-cycles (0..1000), gains in thousandths, tau and dt in milliseconds:
//
//     using HeaterPid = Kvasir::Control::Pid<std::int32_t, std::milli, std::chrono::milliseconds>;
//     inline constexpr HeaterPid::Gains gains{
//       .kp = 300, .ki = 0, .kd = 200, .tau = 700, .outMin = 0, .outMax = 1000};
//     HeaterPid pid{gains};
//     std::int32_t const u = pid.update(setCenti, tipCenti, now - lastAt);   // any duration
//
//   kp 300 is 0.3 half-cycles per C (the 0.01 of input and output cancel), kd 200 is 0.2 s, a ki
//   would be thousandths per second, tau 700 is 0.7 s. An error of 10 C (1000) gives
//   P = 300 * 1000 / 1000 = 300, three half-cycles. A dt of 48 MHz SysTick ticks becomes
//   round(ticks / 48000) ms: the time unit is dt's resolution (+-0.5 ms on 10 ms), so pick a finer
//   one (tau in it too) where that matters.
//
// The arithmetic. The common case is 32-bit throughout: from the gains (at construction and
// setGains()) each factor of P, I and the derivative kick gets its reach - the largest |e|, dt
// and |m - m_prev| that keep its product in 32 bits - and an update inside them multiplies in 32
// bits. Outside, a general path takes int64 products, each saturating at +-2^61 (a sum of three
// stays inside int64: nothing overflows), never a 64-bit multiply with an overflow check. Every
// division rounds and takes 32 bits when its dividend fits them, a 64-bit one only otherwise -
// wideDivisions() counts those of the last update. A compile-time divisor's power of two is a
// shift. On a core without a divide instruction (a Cortex-M0+), a 32-bit division is a multiply by
// the divisor's reciprocal (three 16 x 16-bit products) and at most four corrections, exact; the
// reciprocal of 2 tau + dt is kept until tau or dt changes. Without a long multiply, a 32 x 32-bit
// product with one factor in 16 bits is two 32-bit multiplies. No library call in the common case.
// The general paths are out of line, one copy each; their 64-bit division is four 32-bit ones over
// 16-bit digits for a divisor below 2^16 (every divisor of the example), one bit a step above -
// no 64-bit division routine is linked.
//
// State. The integral is exact, whole + fraction / S output counts with 0 <= fraction < S and
// S = 2 / (GainUnit * TimeUnit) (2'000'000 in the example): ki dt (e + e_prev) is added to the
// fraction and carried (a compare, or a division for an increment beyond S); the anti-windup clamp
// and the rounded integral() are compares. The derivative keeps 8 fraction bits (times the
// denominator of GainUnit / TimeUnit, 1 in the example) and is updated as
// D - (2 kd (m - m_prev) + 2 dt D) / (2 tau + dt), the same filter with a smaller dividend. Per
// update in the example's units: P one division by 125 (after a shift by 3), D one by 2 tau + dt
// and a shift, a duration's conversion (48 MHz ticks to ms) one by 375 (after a shift by 7); the
// integral none. The gains' reaches there: |e| up to 429'496 for kp 5000, dt up to 2'147'483 for
// ki 1000, a measurement step up to 838 per update for kd 5000 (20'971 for kd 200).
//
// Rounding: every division rounds to nearest, ties away from zero (as std::lround). proportional(),
// integral() and derivative() are each within 0.5 counts of their exact value from the same state,
// output() is their clamped sum (within 1.5). The derivative's fraction bits round too: it stays
// within (2 tau + dt) / (1024 dt) counts of the exact filter (0.14 for tau = 700, dt = 10) - a
// decaying D can rest there, reading 0. Against a double Pid on the same inputs (tests): P within
// 0.5 counts, I within 0.5 (1.0 once the anti-windup clamped it: it clamps to outMax - P with P
// rounded), D within 0.5 + (2 tau + dt) / (1024 dt).
//
// Ranges, T = int32 in the example's units (the general terms: the int64 products named above
// within +-2^61). Exact, rounded as above, while
//   - |setpoint - measurement| and each part's value fit T (else each saturates at T's range),
//   - |ki dt| <= 2^29, |kd (m - m_prev)| <= 2^52 and dt <= 2^21 counts (35 min) per update,
// for every other T value of gains, tau and dt (tau < 0 counts as 0), outMin <= outMax. Beyond:
// saturation, never an overflow; a dt from a duration saturates at T's range too.
// The 32-bit paths, same units (a test drives the first user's ranges: kp, kd up to 5000, ki up
// to 1000, tau 10 ms to 5 s, dt 10 or 100 ms from jittered 48 MHz ticks - no 64-bit division):
//   - P: all 32-bit for |kp e| < 2^31; its division in 32 bits for |kp e| < 2^35;
//   - I: all 32-bit inside the reach of ki and for |e + e_prev| < 2^31; its carry divides only for
//     an increment beyond S, in 32 bits while |increment| < 2^39;
//   - D: all 32-bit inside the kick's reach, for |D| < 2^23 counts, dt < 2^14, 0 <= tau < 2^30 and
//     a dividend 512 kd (m - m_prev) + 512 dt D within 32 bits; else its division takes 32 bits
//     while that dividend's magnitude + tau + dt / 2 does (tau >= dt / 2 keeps it there: a ringing
//     D, or a bigger jump, takes one 64-bit division, as exact);
//   - a duration of 48 MHz ticks: all 32-bit below 2^31 ticks (44 s), its division in 32 bits
//     below 2^39 (3 h).
//
// Floating T computes the formula as written. On a Cortex-M0+ (no FPU) each of its ~20 operations
// is a library call - integral T is the form for such a core (a few times faster there); an M33's
// single-precision FPU takes float, never double.

#include <algorithm>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <limits>
#include <ratio>
#include <type_traits>
#include <utility>

namespace Kvasir::Control {

/// The value types of a Pid: floating point, or a signed integer of at most 32 bits (fixed point).
template<typename T>
concept PidValue
  = std::floating_point<T> || (std::signed_integral<T> && sizeof(T) <= sizeof(std::int32_t));

namespace Detail {
    /// The default of a unit an integral Pid has to state.
    struct PidUnitNotGiven {};

    template<typename R>
    inline constexpr bool isPositiveRatio = false;
    template<std::intmax_t N, std::intmax_t D>
    inline constexpr bool isPositiveRatio<std::ratio<N, D>> = std::ratio<N, D>::num > 0;

    template<typename D>
    inline constexpr bool isTimeUnit = false;
    template<typename Rep, typename Period>
    inline constexpr bool isTimeUnit<std::chrono::duration<Rep, Period>> = isPositiveRatio<Period>;

    template<typename GainUnit, typename TimeUnit>
    inline constexpr bool pidUnitsGiven
      = !std::is_same_v<GainUnit, PidUnitNotGiven> && !std::is_same_v<TimeUnit, PidUnitNotGiven>;

    /// The integer arithmetic of a fixed-point Pid: int64 intermediates that saturate at +-limit
    /// (so a sum of three stays inside int64), multiplies without a division (no __mulodi4) and
    /// divisions that round to nearest and take 32 bits when the dividend fits; the narrow forms
    /// work on 32-bit operands throughout.
    namespace PidFixed {
        using Wide  = std::int64_t;
        using UWide = std::uint64_t;

        inline constexpr Wide limit = Wide{1} << 61;

// ACLE: a core with a 32 x 32 -> 64-bit multiply (Thumb-2: SMULL, UMULL) and one with a divide
// instruction. Without them (a Cortex-M0+) each is a library call, so the narrow forms do them
// in 32-bit multiplies instead; the host takes those forms too, so its tests run them.
#if defined(__ARM_ARCH_ISA_THUMB) && __ARM_ARCH_ISA_THUMB >= 2
        inline constexpr bool longMultiply = true;
#else
        inline constexpr bool longMultiply = false;
#endif
#ifdef __ARM_FEATURE_IDIV
        inline constexpr bool divideInstruction = true;
#else
        inline constexpr bool divideInstruction = false;
#endif

        /// |v| without a wrap (also for INT64_MIN)
        [[gnu::always_inline]] constexpr UWide magnitude(Wide v) {
            return v < 0 ? static_cast<UWide>(-(v + 1)) + 1 : static_cast<UWide>(v);
        }

        /// |v| of a 32-bit v, without a wrap
        [[gnu::always_inline]] constexpr std::uint32_t magnitude32(std::int32_t v) {
            return v < 0 ? static_cast<std::uint32_t>(-(v + 1)) + 1U
                         : static_cast<std::uint32_t>(v);
        }

        /// m <= 2^62
        [[gnu::always_inline]] constexpr Wide withSign(UWide m,
                                                       bool  negative) {
            auto const v = static_cast<Wide>(m);
            return negative ? -v : v;
        }

        [[gnu::always_inline]] constexpr Wide saturate(Wide v) {
            return v > limit ? limit : (v < -limit ? -limit : v);
        }

        [[gnu::always_inline]] constexpr bool fits32(UWide v) { return (v >> 32U) == 0; }

        [[gnu::always_inline]] constexpr bool fitsSigned32(Wide v) {
            return v >= std::numeric_limits<std::int32_t>::min()
                && v <= std::numeric_limits<std::int32_t>::max();
        }

        /// a * b / 2^32 for 32-bit a and b, or up to 2 below it: with a long multiply exact (one
        /// UMULL); without, three 16 x 16-bit products, the low halves' product left out (it moves
        /// the high word by at most 1) and the two middle ones each truncated (at most 1 more) - a
        /// form the compiler does not fold back into a 64-bit multiply, which would be a library
        /// call on such a core.
        [[gnu::always_inline]] constexpr std::uint32_t mulHighEstimate(std::uint32_t a,
                                                                       std::uint32_t b) {
            if constexpr(longMultiply) {
                return static_cast<std::uint32_t>((UWide{a} * b) >> 32U);
            } else {
                std::uint32_t const ah = a >> 16U;
                std::uint32_t const bh = b >> 16U;
                return ah * bh + ((ah * (b & 0xFFFFU)) >> 16U) + (((a & 0xFFFFU) * bh) >> 16U);
            }
        }

        /// (2^32 - 1) / d: the reciprocal divide32() takes
        constexpr std::uint32_t reciprocal(std::uint32_t d) { return 0xFFFF'FFFFU / d; }

        /// m / d for 32-bit m and d > 0 on a core without a divide instruction: m times the reciprocal
        /// r = (2^32 - 1) / d and corrections. m r / 2^32 is below m / d and above m / d - 2, the
        /// estimate of it at most 2 lower, so the quotient is reached in at most four steps up -
        /// exact, as the instruction is. Out of line, one copy: smaller and, with its registers to
        /// itself, no slower.
        [[gnu::noinline]] constexpr std::uint32_t divideByReciprocal(std::uint32_t m,
                                                                     std::uint32_t d,
                                                                     std::uint32_t r) {
            std::uint32_t q    = mulHighEstimate(m, r);
            std::uint32_t rest = m - q * d;   // q <= m / d: no wrap
            // nested steps that end at the first rest below d - not a loop: the compiler turns a
            // loop's count back into a division
            if(rest >= d) {
                rest -= d;
                ++q;
                if(rest >= d) {
                    rest -= d;
                    ++q;
                    if(rest >= d) {
                        rest -= d;
                        ++q;
                        if(rest >= d) { ++q; }
                    }
                }
            }
            return q;
        }

        /// m / d for 32-bit m and d > 0 (r: d's reciprocal()): the instruction where there is one,
        /// else divideByReciprocal()
        [[gnu::always_inline]] constexpr std::uint32_t divide32(std::uint32_t m,
                                                                std::uint32_t d,
                                                                std::uint32_t r) {
            if constexpr(divideInstruction) {
                return m / d;
            } else {
                return divideByReciprocal(m, d, r);
            }
        }

        /// a * b for a factor a in 16 bits (|a| <= 2^15, so |a b| <= 2^46): one multiply with a long
        /// multiply; without, two 32-bit ones, b = (b >> 16) 2^16 + (b & 0xFFFF) with
        /// |a (b >> 16)| <= 2^30 and |a (b & 0xFFFF)| < 2^31
        [[gnu::always_inline]] constexpr Wide mul16(std::int32_t a,
                                                    std::int32_t b) {
            if constexpr(longMultiply) {
                return Wide{a} * b;
            } else {
                std::int32_t const high = a * (b >> 16);
                std::int32_t const low  = a * (b & 0xFFFF);
                return Wide{high} * 65536 + low;
            }
        }

        /// a * b, exact (|a b| <= 2^62). Without a long multiply and with a factor in 16 bits:
        /// mul16(); else the 64-bit product (a library call on such a core).
        [[gnu::always_inline]] constexpr Wide mulNarrow(std::int32_t a,
                                                        std::int32_t b) {
            if constexpr(!longMultiply) {
                if(b >= -32768 && b <= 32767) {
                    std::int32_t const t = a;
                    a                    = b;
                    b                    = t;
                }
                if(a >= -32768 && a <= 32767) { return mul16(a, b); }
            }
            return Wide{a} * b;
        }

        /// m / d for a 64-bit m and 0 < d < 2^63 without a 64-bit division routine (on a core without
        /// a divide instruction that is ~0.7 KB of library and ~1000 cycles): for d below 2^16 four
        /// 32-bit divisions over the 16-bit digits of m (each partial dividend is below d 2^16), else
        /// one bit a step. Exact; the general paths' 64-bit division.
        [[gnu::noinline]] constexpr UWide divideLong(UWide m,
                                                     UWide d) {
            // constant shifts only: a variable 64-bit shift is a library call on such a core too
            UWide q{};
            if(d < 0x1'0000U) {
                auto const    divisor = static_cast<std::uint32_t>(d);
                std::uint32_t rest{};
                for(int digit = 0; digit < 4; ++digit) {
                    std::uint32_t const part = (rest << 16U) | static_cast<std::uint32_t>(m >> 48U);
                    std::uint32_t const qd   = part / divisor;
                    rest                     = part - qd * divisor;
                    q                        = (q << 16U) | qd;
                    m = (m & 0x0000'FFFF'FFFF'FFFFU) << 16U;   // the digit taken off first
                }
                return q;
            }
            UWide rest{};
            for(int bit = 0; bit < 64; ++bit) {
                rest = (rest << 1U) | (m >> 63U);   // rest < d < 2^63: no overflow
                m    = (m & 0x7FFF'FFFF'FFFF'FFFFU) << 1U;
                q <<= 1U;
                if(rest >= d) {
                    rest -= d;
                    q |= 1U;
                }
            }
            return q;
        }

        /// |a| * |b| for factors beyond 32 bits: the larger split in 32-bit halves, saturated -
        /// never a division (no __mulodi4).
        [[gnu::noinline]] constexpr Wide mulLarge(Wide a,
                                                  Wide b) {
            UWide      small    = magnitude(a);
            UWide      large    = magnitude(b);
            bool const negative = (a < 0) != (b < 0);
            if(small > large) {
                UWide const t = small;
                small         = large;
                large         = t;
            }
            UWide product{};
            if(fits32(large)) {
                product = small * large;   // both < 2^32
            } else if(fits32(small)) {
                // large = high 2^32 + low: each part times small is a 32 x 32-bit product; high >= small
                // (large >= 2^32), so below 2^29 both halves are below 2^61 and their sum fits
                UWide const high = (large >> 32U) * small;
                if((high >> 29U) != 0) { return negative ? -limit : limit; }
                product = (high << 32U) + (large & 0xFFFF'FFFFU) * small;
            } else {
                return negative ? -limit : limit;   // both >= 2^32
            }
            return product > static_cast<UWide>(limit) ? (negative ? -limit : limit)
                                                       : withSign(product, negative);
        }

        /// a * b, saturated to +-limit: mulNarrow() for two 32-bit factors, else mulLarge(). Out of
        /// line, one copy: it serves the general paths and the reaches only.
        [[gnu::noinline]] constexpr Wide mul(Wide a,
                                             Wide b) {
            if(fitsSigned32(a) && fitsSigned32(b)) {
                return saturate(
                  mulNarrow(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b)));
            }
            return mulLarge(a, b);
        }

        /// x * N for a compile-time N > 0, saturated
        template<std::intmax_t N>
        [[gnu::always_inline]] constexpr Wide mulBy(Wide x) {
            static_assert(N > 0);
            if constexpr(N == 1) {
                return x;
            } else if constexpr(std::has_single_bit(static_cast<UWide>(N))) {
                constexpr Wide reach = limit / N;   // a shift, saturated
                return x > reach ? limit : (x < -reach ? -limit : x * N);
            } else {
                return mul(x, Wide{N});
            }
        }

        /// n / d rounded to nearest, ties away from zero, for d > 0 and |n| <= 2^62, the d's factor
        /// 2^shift first a shift (a floor of a floor is the floor: (m >> k) / odd is m / d, and m
        /// carries the half): a 32-bit division when the shifted magnitude and the odd rest fit 32
        /// bits, else a 64-bit one, counted in `wide`. The general paths' division, out of line.
        [[gnu::noinline]] constexpr Wide divide(Wide          n,
                                                UWide         d,
                                                int           shift,
                                                std::uint8_t& wide) {
            UWide const odd = d >> shift;
            UWide       m   = (magnitude(n) + d / 2) >> shift;
            if(odd != 1) {
                if(fits32(m) && fits32(odd)) {
                    m = static_cast<std::uint32_t>(m) / static_cast<std::uint32_t>(odd);
                } else {
                    ++wide;
                    m = divideLong(m, odd);
                }
            }
            return withSign(m, n < 0);
        }

        /// n / d rounded, for a 32-bit n and 0 < d < 2^32 (r: its reciprocal()): all in 32 bits
        [[gnu::always_inline]] constexpr Wide divideNarrow(std::int32_t  n,
                                                           std::uint32_t d,
                                                           std::uint32_t r) {
            auto const q = Wide{divide32(magnitude32(n) + d / 2, d, r)};   // 2^31 + d / 2 < 2^32
            return n < 0 ? -q : q;
        }

        /// n / D for a compile-time D: divide() with D's power of two as its shift
        template<std::intmax_t D>
        [[gnu::always_inline]] constexpr Wide divide(Wide          n,
                                                     std::uint8_t& wide) {
            static_assert(D > 0);
            if constexpr(D == 1) {
                return n;
            } else {
                return divide(n,
                              static_cast<UWide>(D),
                              std::countr_zero(static_cast<UWide>(D)),
                              wide);
            }
        }

        /// n / D rounded, for a 32-bit n and a compile-time D below 2^32: all in 32 bits
        /// (|n| + D / 2 < 2^32; the quotient fits 32 bits for D >= 2)
        template<std::intmax_t D>
        [[gnu::always_inline]] constexpr std::int32_t divideNarrow(std::int32_t n) {
            static_assert(D > 0 && D < (std::intmax_t{1} << 32));
            if constexpr(D == 1) {
                return n;
            } else {
                constexpr auto          d     = static_cast<std::uint32_t>(D);
                constexpr int           shift = std::countr_zero(d);
                constexpr std::uint32_t odd   = d >> shift;
                std::uint32_t           m     = (magnitude32(n) + d / 2) >> shift;
                if constexpr(odd != 1) {
                    constexpr std::uint32_t r = reciprocal(odd);
                    m                         = divide32(m, odd, r);
                }
                auto const q = static_cast<std::int32_t>(m);
                return n < 0 ? -q : q;
            }
        }

        [[gnu::always_inline]] constexpr std::int32_t saturate32(Wide v) {
            return static_cast<std::int32_t>(v > std::numeric_limits<std::int32_t>::max()
                                               ? std::numeric_limits<std::int32_t>::max()
                                             : v < std::numeric_limits<std::int32_t>::min()
                                               ? std::numeric_limits<std::int32_t>::min()
                                               : v);
        }

        struct FloorDivided {
            Wide quotient;
            Wide rest;   // 0 <= rest < D
        };

        /// x = quotient d + rest, 0 <= rest < d, for d > 0 and |x| < 2^63: d's factor 2^shift a
        /// shift, the odd rest a 32-bit division when the shifted magnitude fits, else a 64-bit
        /// one, counted in `wide`. Out of line: it carries a large integral increment only.
        [[gnu::noinline]] constexpr FloorDivided floorDivide(Wide          x,
                                                             UWide         d,
                                                             int           shift,
                                                             std::uint8_t& wide) {
            UWide const odd = d >> shift;
            // floor(x / d) for x >= 0; for x < 0 -ceil(|x| / d) = -floor((|x| + d - 1) / d)
            UWide m = (magnitude(x) + (x < 0 ? d - 1 : 0)) >> shift;
            if(odd != 1) {
                if(fits32(m) && fits32(odd)) {
                    m = static_cast<std::uint32_t>(m) / static_cast<std::uint32_t>(odd);
                } else {
                    ++wide;
                    m = divideLong(m, odd);
                }
            }
            Wide const q = withSign(m, x < 0);
            return {q, x - q * static_cast<Wide>(d)};
        }

        /// a - b and a + b in 32 bits, false when they do not fit
        template<typename T>
        [[gnu::always_inline]] constexpr bool subtract32(T             a,
                                                         T             b,
                                                         std::int32_t& out) {
            if constexpr(sizeof(T) < sizeof(std::int32_t)) {
                out = a - b;
                return true;
            } else {
                return !__builtin_sub_overflow(a, b, &out);
            }
        }

        template<typename T>
        [[gnu::always_inline]] constexpr bool add32(T             a,
                                                    T             b,
                                                    std::int32_t& out) {
            if constexpr(sizeof(T) < sizeof(std::int32_t)) {
                out = a + b;
                return true;
            } else {
                return !__builtin_add_overflow(a, b, &out);
            }
        }

        /// A factor of the narrow paths and its reach: |x| <= reach keeps factor x in 32 bits. A
        /// factor beyond 32 bits is 0 with reach 0 - only x = 0 takes the narrow path, and gets 0.
        struct Reach {
            std::int32_t  factor{};
            std::uint32_t reach{};

            static constexpr Reach of(Wide f) {
                if(!fitsSigned32(f) || f == std::numeric_limits<std::int32_t>::min()) { return {}; }
                auto const narrow = static_cast<std::int32_t>(f);
                return {narrow, narrow == 0 ? 0x7FFF'FFFFU : 0x7FFF'FFFFU / magnitude32(narrow)};
            }

            [[nodiscard]] constexpr bool holds(std::int32_t x) const {
                return magnitude32(x) <= reach;
            }
        };
    }   // namespace PidFixed
}   // namespace Detail

/// A floating T needs no units; an integral one states them (see the top of the file).
template<typename T>
using DefaultPidGainUnit
  = std::conditional_t<std::floating_point<T>, std::ratio<1>, Detail::PidUnitNotGiven>;

template<typename T>
using DefaultPidTimeUnit
  = std::conditional_t<std::floating_point<T>, std::chrono::duration<T>, Detail::PidUnitNotGiven>;

template<PidValue T,
         typename GainUnit = DefaultPidGainUnit<T>,
         typename TimeUnit = DefaultPidTimeUnit<T>>
struct PidGains {
    static_assert(Detail::pidUnitsGiven<GainUnit,
                                        TimeUnit>,
                  "an integral Kvasir::Control::PidGains states its gain unit and time unit, e.g. "
                  "PidGains<std::int32_t, std::milli, std::chrono::milliseconds>");
    T kp{};
    T ki{};
    T kd{};
    T tau{};   // derivative filter time constant, in TimeUnit counts (seconds by default)
    T outMin{};
    T outMax{};
};

template<typename T,
         typename GainUnit = DefaultPidGainUnit<T>,
         typename TimeUnit = DefaultPidTimeUnit<T>>
class Pid {
    static_assert(
      PidValue<T>,
      "Kvasir::Control::Pid takes a floating type or a signed integer of at most 32 bits");
    static_assert(Detail::pidUnitsGiven<GainUnit,
                                        TimeUnit>,
                  "an integral Kvasir::Control::Pid states its gain unit and time unit, e.g. "
                  "Pid<std::int32_t, std::milli, std::chrono::milliseconds>");
    static_assert(Detail::isPositiveRatio<GainUnit>,
                  "GainUnit is a std::ratio > 0: the value of one count of kp, ki and kd");
    static_assert(
      Detail::isTimeUnit<TimeUnit>,
      "TimeUnit is a std::chrono::duration type: the length of one count of tau and dt");

    static constexpr bool floating = std::floating_point<T>;

    using Wide  = Detail::PidFixed::Wide;
    using UWide = Detail::PidFixed::UWide;
    using Reach = Detail::PidFixed::Reach;
    using Sum   = std::conditional_t<floating, T, Wide>;

    using Seconds = typename TimeUnit::period;   // one count of tau and dt
    // P = kp e PRatio, I += ki dt (e + e_prev) / 2 IRatio, D on kd DRatio per count of time
    using PRatio = GainUnit;
    using IRatio = std::ratio_multiply<GainUnit, Seconds>;
    using DRatio = std::ratio_divide<GainUnit, Seconds>;

    // fixed point: the integral counts 1 / integralScale of an output count, the derivative
    // 1 / derivativeScale; derivativeKick turns kd (m - m_prev) into the latter
    static constexpr int           derivativeFractionBits = 8;
    static constexpr std::intmax_t integralScale          = 2 * IRatio::den;
    static constexpr std::intmax_t derivativeScale        = DRatio::den << derivativeFractionBits;
    static constexpr std::intmax_t derivativeKick = (2 * DRatio::num) << derivativeFractionBits;
    static_assert(floating
                    || (IRatio::den <= (std::intmax_t{1} << 60)
                        && DRatio::den <= (std::intmax_t{1} << (62 - derivativeFractionBits))
                        && DRatio::num <= (std::intmax_t{1} << (61 - derivativeFractionBits))),
                  "GainUnit and TimeUnit too far apart for the fixed point");
    // the narrow paths divide by these in 32 bits
    static constexpr bool narrowP = PRatio::den < (std::intmax_t{1} << 32);
    static constexpr bool narrowD = derivativeScale < (std::intmax_t{1} << 32);
    // the integral's fraction in 32 bits: below 2^30, so fraction + increment fits for |increment| < scale
    static constexpr bool narrowFraction = integralScale <= (std::intmax_t{1} << 30);
    using Fraction                       = std::conditional_t<narrowFraction, std::int32_t, Wide>;

public:
    using Gains = PidGains<T, GainUnit, TimeUnit>;

    constexpr explicit Pid(Gains g) : g_{g} { reach(); }

    /// dt in TimeUnit counts (seconds for a floating T's default); dt <= 0 holds I and D.
    constexpr T update(T setpoint,
                       T measurement,
                       T dt) {
        wide_ = 0;
        return step(setpoint, measurement, dt);
    }

    /// dt as any duration, converted to TimeUnit counts as count * (Period / TimeUnit::period):
    /// floating T one multiply by that ratio (no division), integral T rounded to nearest with the
    /// ratio's power of two a shift and the rest a 32-bit division while the shifted count fits.
    template<typename Rep,
             typename Period>
    constexpr T update(T                             setpoint,
                       T                             measurement,
                       std::chrono::duration<Rep,
                                             Period> dt) {
        wide_          = 0;
        T const counts = countsOf(dt);
        return step(setpoint, measurement, counts);
    }

    /// The next update is P only again (tip changed, heater off).
    constexpr void reset() {
        primed_ = false;
        i_      = T{};
        d_      = T{};
        fixed_  = {};
    }

    constexpr void setGains(Gains g) {
        g_ = g;
        if constexpr(floating) {
            i_ = std::clamp(i_, g.outMin, g.outMax);
        } else {
            clampIntegral(Wide{g.outMin}, Wide{g.outMax});
            i_ = integralValue();
            reach();
        }
    }

    [[nodiscard]] constexpr Gains const& gains() const { return g_; }

    [[nodiscard]] constexpr T proportional() const { return p_; }

    [[nodiscard]] constexpr T integral() const { return i_; }

    [[nodiscard]] constexpr T derivative() const { return d_; }

    [[nodiscard]] constexpr T output() const { return out_; }

    /// Divisions the last update did in 64 bits (0 in the common case; always 0 for a floating T).
    [[nodiscard]] constexpr std::uint8_t wideDivisions() const { return wide_; }

private:
    // the algorithm, one for both kinds; the arithmetic of each step below
    constexpr T step(T setpoint,
                     T measurement,
                     T dt) {
        T const error = errorOf(setpoint, measurement);
        p_            = proportionalOf(error);
        Sum out       = p_;
        if(primed_ && dt > T{}) {
            integrate(error, dt);
            filterDerivative(measurement, dt);
            out += Sum{i_} + Sum{d_};
        }
        primed_          = true;
        prevMeasurement_ = measurement;
        prevError_       = error;
        return out_      = static_cast<T>(std::clamp(out, Sum{g_.outMin}, Sum{g_.outMax}));
    }

    static constexpr Wide lowest  = std::numeric_limits<T>::min();
    static constexpr Wide highest = std::numeric_limits<T>::max();

    static constexpr T saturated(Wide v) {
        return static_cast<T>(v > highest ? highest : (v < lowest ? lowest : v));
    }

    /// x * R for a floating T, nothing for R = 1
    template<typename R>
    static constexpr T scaled(T x) {
        if constexpr(R::num == 1 && R::den == 1) {
            return x;
        } else {
            // in double: the same constant on every host (a long double is 80 bits on one, 64 on another)
            return x * static_cast<T>(static_cast<double>(R::num) / static_cast<double>(R::den));
        }
    }

    /// the factors of the narrow paths and how far they reach, from the gains
    constexpr void reach() {
        if constexpr(!floating) {
            namespace Fx        = Detail::PidFixed;
            reach_.proportional = Reach::of(Fx::mulBy<PRatio::num>(Wide{g_.kp}));
            reach_.integral     = Reach::of(Fx::mulBy<IRatio::num>(Wide{g_.ki}));
            reach_.kick         = Reach::of(Fx::mulBy<derivativeKick>(Wide{g_.kd}));
        }
    }

    static constexpr T errorOf(T setpoint,
                               T measurement) {
        if constexpr(floating) {
            return setpoint - measurement;
        } else {
            std::int32_t error{};
            if(Detail::PidFixed::subtract32(setpoint, measurement, error)) {
                return saturated(error);
            }
            return setpoint < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
        }
    }

    constexpr T proportionalOf(T error) {
        if constexpr(floating) {
            return scaled<PRatio>(g_.kp * error);
        } else {
            namespace Fx = Detail::PidFixed;
            if constexpr(narrowP) {
                if(reach_.proportional.holds(error)) {
                    return saturated(
                      Fx::divideNarrow<PRatio::den>(reach_.proportional.factor * error));
                }
            }
            return proportionalWide(error);
        }
    }

    [[gnu::noinline]] constexpr T proportionalWide(T error) {
        namespace Fx = Detail::PidFixed;
        return saturated(
          Fx::divide<PRatio::den>(Fx::mulBy<PRatio::num>(Fx::mul(g_.kp, error)), wide_));
    }

    // trapezoid, then anti-windup: the integral gets only the room P leaves inside the limits
    constexpr void integrate(T error,
                             T dt) {
        if constexpr(floating) {
            i_ += scaled<IRatio>(T{0.5} * g_.ki * dt * (error + prevError_));
            T const iMax = g_.outMax > p_ ? g_.outMax - p_ : T{};
            T const iMin = g_.outMin < p_ ? g_.outMin - p_ : T{};
            i_           = std::clamp(i_, iMin, iMax);
        } else {
            namespace Fx = Detail::PidFixed;
            if(g_.ki != 0) {
                // ki dt (e + e_prev) num(GainUnit * TimeUnit) in 1 / integralScale counts
                std::int32_t sum{};
                if(reach_.integral.holds(dt) && Fx::add32(error, prevError_, sum)) {
                    addToIntegral(Fx::mulNarrow(reach_.integral.factor * dt, sum));   // |.| <= 2^62
                } else {
                    addToIntegral(integralIncrementWide(error, dt));
                }
            }
            if(fixed_.whole == 0 && fixed_.fraction == 0) {
                i_ = T{};   // the room always holds 0
                return;
            }
            clampIntegral(g_.outMin < p_ ? Wide{g_.outMin} - p_ : 0,
                          g_.outMax > p_ ? Wide{g_.outMax} - p_ : 0);
            i_ = integralValue();
        }
    }

    // the integral is whole + fraction / integralScale counts, 0 <= fraction < integralScale: an
    // increment below integralScale is an add and one carry, the clamp and the rounded value are
    // compares - no multiply, no division
    constexpr void addToIntegral(Wide increment) {
        namespace Fx = Detail::PidFixed;
        if constexpr(narrowFraction) {
            if(increment > -integralScale && increment < integralScale) {
                auto f = static_cast<std::int32_t>(fixed_.fraction
                                                   + increment);   // in (-scale, 2 scale)
                if(f >= integralScale) {
                    f -= integralScale;
                    fixed_.whole = Fx::saturate32(Wide{fixed_.whole} + 1);
                } else if(f < 0) {
                    f += integralScale;
                    fixed_.whole = Fx::saturate32(Wide{fixed_.whole} - 1);
                }
                fixed_.fraction = f;
                return;
            }
        }
        carryIntegral(increment);
    }

    [[nodiscard,
      gnu::noinline]] constexpr Wide
    integralIncrementWide(T error,
                          T dt) const {
        namespace Fx = Detail::PidFixed;
        return Fx::mulBy<IRatio::num>(Fx::mul(Fx::mul(g_.ki, dt), Wide{error} + Wide{prevError_}));
    }

    // |fraction + increment| < 2^63: the carry by a floor division
    [[gnu::noinline]] constexpr void carryIntegral(Wide increment) {
        namespace Fx = Detail::PidFixed;
        auto const [carry, rest]
          = Fx::floorDivide(Wide{fixed_.fraction} + increment,
                            static_cast<UWide>(integralScale),
                            std::countr_zero(static_cast<UWide>(integralScale)),
                            wide_);
        fixed_.fraction = static_cast<Fraction>(rest);
        fixed_.whole    = Fx::saturate32(Wide{fixed_.whole} + carry);
    }

    /// the integral into [low, high] (low <= high, whole numbers)
    constexpr void clampIntegral(Wide low,
                                 Wide high) {
        if(fixed_.whole > high || (fixed_.whole == high && fixed_.fraction > 0)) {
            fixed_.whole    = Detail::PidFixed::saturate32(high);
            fixed_.fraction = 0;
        } else if(fixed_.whole < low) {
            fixed_.whole    = Detail::PidFixed::saturate32(low);
            fixed_.fraction = 0;
        }
    }

    /// the integral rounded to nearest, ties away from zero
    [[nodiscard]] constexpr T integralValue() const {
        Wide const twice = 2 * Wide{fixed_.fraction};
        bool const up    = fixed_.whole >= 0 ? twice >= integralScale : twice > integralScale;
        return saturated(Wide{fixed_.whole} + (up ? 1 : 0));
    }

    constexpr void filterDerivative(T measurement,
                                    T dt) {
        if constexpr(floating) {
            d_ = (-(T{2} * scaled<DRatio>(g_.kd) * (measurement - prevMeasurement_))
                  + (T{2} * g_.tau - dt) * d_)
               / (T{2} * g_.tau + dt);
        } else {
            // D - (2 kd (m - m_prev) + 2 dt D) / (2 tau + dt): the same filter rearranged, so the
            // dividend holds dt D instead of (2 tau - dt) D and stays in 32 bits longer
            namespace Fx       = Detail::PidFixed;
            Wide&        state = fixed_.derivative;
            std::int32_t rise{};
            bool const   narrowRise
              = Fx::subtract32(measurement, prevMeasurement_, rise) && reach_.kick.holds(rise);
            if(narrowRise && (rise == 0 || g_.kd == 0) && state == 0) { return; }
            if constexpr(narrowD) {
                // the common case in 32 bits: a 32-bit D, dt below 2^14 (2 dt D < 2^46), 0 <= tau
                // below 2^30, a 32-bit dividend
                if(narrowRise && Fx::fitsSigned32(state) && dt <= 16383 && g_.tau >= 0
                   && g_.tau < (std::int32_t{1} << 30))
                {
                    auto const den
                      = static_cast<std::uint32_t>(2 * static_cast<std::int32_t>(g_.tau) + dt);
                    refreshReciprocal(den);
                    auto const   s = static_cast<std::int32_t>(state);
                    Wide const   n = Wide{reach_.kick.factor * rise}
                                   + Fx::mul16(2 * static_cast<std::int32_t>(dt), s);
                    std::int32_t next{};
                    if(Fx::fitsSigned32(n)
                       && Fx::subtract32(
                         s,
                         static_cast<std::int32_t>(
                           Fx::divideNarrow(static_cast<std::int32_t>(n), den, fixed_.reciprocal)),
                         next))
                    {
                        state = next;
                        d_    = saturated(Fx::divideNarrow<derivativeScale>(next));
                        return;
                    }
                }
            }
            filterDerivativeWide(measurement, dt);
        }
    }

    // the reciprocal of 2 tau + dt for divide32(), kept while tau and dt stay (a division each change)
    constexpr void refreshReciprocal(UWide den) {
        if constexpr(!Detail::PidFixed::divideInstruction) {
            if(Detail::PidFixed::fits32(den) && den != fixed_.denominator) {
                fixed_.denominator = static_cast<std::uint32_t>(den);
                fixed_.reciprocal  = Detail::PidFixed::reciprocal(fixed_.denominator);
            }
        }
    }

    [[gnu::noinline]] constexpr void filterDerivativeWide(T measurement,
                                                          T dt) {
        namespace Fx     = Detail::PidFixed;
        Wide&      state = fixed_.derivative;
        Wide const tau   = g_.tau > 0 ? Wide{g_.tau} : 0;
        auto const den   = static_cast<UWide>(2 * tau + Wide{dt});
        Wide const kick
          = Fx::mulBy<derivativeKick>(Fx::mul(g_.kd, Wide{measurement} - Wide{prevMeasurement_}));
        Wide const decay = Fx::mul(2 * Wide{dt}, state);
        state            = Fx::saturate(state - Fx::divide(kick + decay, den, 0, wide_));
        if constexpr(narrowD) {
            if(Fx::fitsSigned32(state)) {
                d_ = saturated(Fx::divideNarrow<derivativeScale>(static_cast<std::int32_t>(state)));
                return;
            }
        }
        d_ = saturated(Fx::divide<derivativeScale>(state, wide_));
    }

    template<typename Rep,
             typename Period>
    constexpr T countsOf(std::chrono::duration<Rep,
                                               Period> dt) {
        using Ratio   = std::ratio_divide<Period, Seconds>;   // TimeUnit counts per source count
        Rep const raw = dt.count();
        if constexpr(floating) {
            if constexpr(std::is_integral_v<Rep>) {
                // a count that fits 32 bits converts as one: a 64-bit integer to float is the
                // slower library call on a core without an FPU
                T const counts = std::in_range<std::int32_t>(raw)
                                 ? static_cast<T>(static_cast<std::int32_t>(raw))
                                 : static_cast<T>(raw);
                return scaled<Ratio>(counts);
            } else {
                return scaled<Ratio>(static_cast<T>(raw));
            }
        } else if constexpr(std::is_integral_v<Rep>) {
            namespace Fx = Detail::PidFixed;
            if constexpr(Ratio::num == 1 && Ratio::den < (std::intmax_t{1} << 31)) {
                // a count below 2^31: (count + den / 2) in 32 bits
                if(std::cmp_greater_equal(raw, 0) && std::cmp_less(raw, std::intmax_t{1} << 31)) {
                    return saturated(Fx::divideNarrow<Ratio::den>(static_cast<std::int32_t>(raw)));
                }
            }
            Wide const counted = std::cmp_greater(raw, Fx::limit) ? Fx::limit
                               : std::cmp_less(raw, -Fx::limit)   ? -Fx::limit
                                                                  : static_cast<Wide>(raw);
            return saturated(Fx::divide<Ratio::den>(Fx::mulBy<Ratio::num>(counted), wide_));
        } else {
            // a floating count: rounded to nearest, ties away from zero, in double
            double const   v  = static_cast<double>(raw)
                              * (static_cast<double>(Ratio::num) / static_cast<double>(Ratio::den));
            constexpr auto lo = static_cast<double>(std::numeric_limits<T>::min());
            constexpr auto hi = static_cast<double>(std::numeric_limits<T>::max());
            if(!(v > lo)) { return std::numeric_limits<T>::min(); }   // NaN too: holds I and D
            if(v >= hi) { return std::numeric_limits<T>::max(); }
            return static_cast<T>(v < 0.0 ? v - 0.5 : v + 0.5);
        }
    }

    struct FloatState {};

    struct FixedState {
        std::int32_t  whole{};      // the integral: whole + fraction / integralScale output counts,
        Fraction      fraction{};   // 0 <= fraction < integralScale
        Wide          derivative{};    // 1 / derivativeScale output counts
        std::uint32_t denominator{};   // 2 tau + dt of the last update, and its reciprocal()
        std::uint32_t reciprocal{};
    };

    // the factors of the narrow paths, from the gains: kp GainUnit::num, ki num(GainUnit * TimeUnit)
    // and 2^9 num(GainUnit / TimeUnit) kd
    struct FixedReach {
        Reach proportional{};
        Reach integral{};
        Reach kick{};
    };

    Gains                                                                      g_;
    T                                                                          p_{};
    T                                                                          i_{};
    T                                                                          d_{};
    T                                                                          out_{};
    T                                                                          prevError_{};
    T                                                                          prevMeasurement_{};
    bool                                                                       primed_{};
    std::uint8_t                                                               wide_{};
    [[no_unique_address]] std::conditional_t<floating, FloatState, FixedState> fixed_{};
    [[no_unique_address]] std::conditional_t<floating, FloatState, FixedReach> reach_{};
};

}   // namespace Kvasir::Control
