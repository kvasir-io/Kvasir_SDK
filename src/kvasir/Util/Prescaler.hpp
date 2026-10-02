#pragma once
// Clock in, rate wanted: the divider settings, and a tolerance check that prints the numbers.
//
//     namespace P = Kvasir::Prescaler;
//     constexpr auto r = P::fromRange(48'000'000, 7'000'000, {.first = 1, .last = 256});
//     // r.setting == 7, r.achieved == P::Rational{48'000'000, 7}
//     constexpr auto u = P::fromFixedPoint(150'000'000, 115'200,
//                                          {.intMin = 1, .intMax = 65535, .fracBits = 6, .scale = 16});
//     // the PL011: divisor 81 + 24/64; a divisor of x.995 gives {x + 1, 0}, never {x, 64}
//     P::assertInTolerance<u.achieved, 115'200, P::Tolerance{std::ratio<5, 1000>{}}, "UART0 baud rate">();
//     // error: static assertion failed: UART0 baud rate: wanted 115200 Hz, got 117187.5 Hz
//     //        (+17253 ppm), allowed 5000 ppm
//
// Everything is exact integer arithmetic (rates as fractions, cross-multiplied in 64 bits); a
// product that would overflow is a compile error, never a wrap. Ties keep the first candidate in
// iteration order.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <ratio>
#include <span>
#include <string_view>
#include <utility>

namespace Kvasir::Prescaler {

namespace detail {
    // not constexpr: reaching it in a constant expression is the compile error
    inline std::uint64_t prescalerArithmeticOverflowsSixtyFourBits(std::uint64_t a,
                                                                   std::uint64_t b) {
        return a * b;
    }
}   // namespace detail

constexpr std::uint64_t mulChecked(std::uint64_t a,
                                   std::uint64_t b) {
    std::uint64_t r{};
    if(__builtin_mul_overflow(a, b, &r)) {
        return detail::prescalerArithmeticOverflowsSixtyFourBits(a, b);
    }
    return r;
}

// An exact rate: num / den Hz. The achieved rate of a divider is almost never an integer.
struct Rational {
    std::uint64_t num{};
    std::uint64_t den{1};

    constexpr bool operator==(Rational const&) const = default;
};

// Relative tolerance num / den; from a std::ratio so every existing config keeps its spelling.
struct Tolerance {
    std::uint64_t num{};
    std::uint64_t den{1};

    template<std::intmax_t N,
             std::intmax_t D>
    consteval Tolerance(std::ratio<N,
                                   D>)   // NOLINT(google-explicit-constructor)
      : num{static_cast<std::uint64_t>(N)}
      , den{static_cast<std::uint64_t>(D)} {
        static_assert(N >= 0 && D > 0);
    }

    consteval Tolerance(std::uint64_t n,
                        std::uint64_t d)
      : num{n}
      , den{d} {}

    static consteval Tolerance ppm(std::uint64_t p) { return {p, 1'000'000}; }
};

enum class Pick : std::uint8_t {
    nearest,    // smallest |achieved - wanted|
    notAbove,   // closest with achieved <= wanted; none: nearest
    notBelow,   // closest with achieved >= wanted; none: nearest
    firstFit,   // the first candidate, in iteration order, that is not above
};

// |achieved - wanted| as a fraction over achieved.den
constexpr std::uint64_t distanceNum(Rational      a,
                                    std::uint64_t wanted) {
    auto const w = mulChecked(wanted, a.den);
    return a.num > w ? a.num - w : w - a.num;
}

// a closer to wanted than b
constexpr bool closer(Rational      a,
                      Rational      b,
                      std::uint64_t wanted) {
    return mulChecked(distanceNum(a, wanted), b.den) < mulChecked(distanceNum(b, wanted), a.den);
}

constexpr bool above(Rational      a,
                     std::uint64_t wanted) {
    return a.num > mulChecked(wanted, a.den);
}

constexpr bool below(Rational      a,
                     std::uint64_t wanted) {
    return a.num < mulChecked(wanted, a.den);
}

// signed error, rounded to nearest, in parts per `scale` of wanted
constexpr std::int64_t errorIn(Rational      a,
                               std::uint64_t wanted,
                               std::uint64_t scale) {
    auto const d   = distanceNum(a, wanted);   // |a - w| * a.den
    auto const den = mulChecked(wanted, a.den);
    auto const v   = (mulChecked(d, scale) + den / 2) / den;
    return above(a, wanted) ? static_cast<std::int64_t>(v) : -static_cast<std::int64_t>(v);
}

template<typename Setting>
struct Result {
    Setting  setting{};
    Rational achieved{};
    bool     found{};

    constexpr std::int64_t errorPpm(std::uint64_t wanted) const {
        return errorIn(achieved, wanted, 1'000'000);
    }

    constexpr std::int64_t errorPpb(std::uint64_t wanted) const {
        return errorIn(achieved, wanted, 1'000'000'000);
    }
};

// |achieved - wanted| <= wanted * tolerance, exactly
constexpr bool inTolerance(Rational      achieved,
                           std::uint64_t wanted,
                           Tolerance     tol) {
    return mulChecked(distanceNum(achieved, wanted), tol.den)
        <= mulChecked(mulChecked(wanted, tol.num), achieved.den);
}

// Candidates: any forward range of Setting. factor: Setting -> the Rational the input clock is
// multiplied by. Pick decides; on an exact tie prefer(a, b) says whether candidate a replaces the
// one kept so far, b (the RP PLL: the higher VCO); without it the first candidate stays.
template<std::ranges::forward_range Cs,
         typename F,
         typename Prefer>
constexpr auto search(std::uint64_t in,
                      std::uint64_t want,
                      Cs&&          candidates,
                      F&&           factor,
                      Pick          pick,
                      Prefer&&      prefer) {
    using Setting = std::ranges::range_value_t<Cs>;
    Result<Setting> best{};
    Result<Setting> nearest{};
    auto const      better = [&](Rational a, Setting const& c, Result<Setting> const& kept) {
        if(!kept.found || closer(a, kept.achieved, want)) { return true; }
        return !closer(kept.achieved, a, want) && prefer(c, kept.setting);
    };
    for(auto const& c : candidates) {
        Rational const f = factor(c);
        Rational const a{mulChecked(in, f.num), f.den};
        if(better(a, c, nearest)) { nearest = {c, a, true}; }
        bool const ok = pick == Pick::notAbove || pick == Pick::firstFit ? !above(a, want)
                      : pick == Pick::notBelow                           ? !below(a, want)
                                                                         : true;
        if(!ok) { continue; }
        if(pick == Pick::firstFit) { return Result<Setting>{c, a, true}; }
        if(better(a, c, best)) { best = {c, a, true}; }
    }
    return best.found ? best : nearest;
}

template<std::ranges::forward_range Cs,
         typename F>
constexpr auto search(std::uint64_t in,
                      std::uint64_t want,
                      Cs&&          candidates,
                      F&&           factor,
                      Pick          pick = Pick::nearest) {
    return search(in,
                  want,
                  std::forward<Cs>(candidates),
                  std::forward<F>(factor),
                  pick,
                  [](auto const&, auto const&) { return false; });
}

// divide by n, first..last
struct Linear {
    std::uint32_t first;
    std::uint32_t last;
};

// divide by 2^e, minExp..maxExp
struct Pow2 {
    unsigned minExp;
    unsigned maxExp;
};

// a divisor integer + fraction / 2^fracBits, times scale (the PL011: scale 16, 6 fraction bits)
struct Fixed {
    std::uint32_t intMin;
    std::uint32_t intMax;
    unsigned      fracBits;
    std::uint32_t scale = 1;
};

// what goes into the registers
struct IntFrac {
    std::uint32_t integer{};
    std::uint32_t fraction{};

    constexpr bool operator==(IntFrac const&) const = default;
};

namespace detail {
    // the two candidates around in / (scale * want) in steps of 1/unit, clamped to [lo, hi]
    constexpr std::array<std::uint64_t,
                         2>
    neighbours(std::uint64_t in,
               std::uint64_t want,
               std::uint64_t unit,
               std::uint64_t scale,
               std::uint64_t lo,
               std::uint64_t hi) {
        auto const num   = mulChecked(in, unit);
        auto const den   = mulChecked(want, scale);
        auto const floor = num / den;
        auto const ceil  = floor + (num % den != 0 ? 1U : 0U);
        return {std::clamp(floor, lo, hi), std::clamp(ceil, lo, hi)};
    }
}   // namespace detail

constexpr auto fromRange(std::uint64_t in,
                         std::uint64_t want,
                         Linear        r,
                         Pick          pick = Pick::nearest) {
    auto const n = detail::neighbours(in, want, 1, 1, r.first, r.last);
    // floor first: on a tie the smaller divider wins, as a loop from `first` would
    std::array<std::uint32_t, 2> const cs{static_cast<std::uint32_t>(n[0]),
                                          static_cast<std::uint32_t>(n[1])};
    return search(in, want, cs, [](std::uint32_t d) { return Rational{1, d}; }, pick);
}

constexpr auto fromList(std::uint64_t                  in,
                        std::uint64_t                  want,
                        std::span<std::uint32_t const> dividers,
                        Pick                           pick = Pick::nearest) {
    return search(in, want, dividers, [](std::uint32_t d) { return Rational{1, d}; }, pick);
}

constexpr auto fromPowerOfTwo(std::uint64_t in,
                              std::uint64_t want,
                              Pow2          p,
                              Pick          pick = Pick::nearest) {
    return search(
      in,
      want,
      std::views::iota(p.minExp, p.maxExp + 1),
      [](unsigned e) { return Rational{1, std::uint64_t{1} << e}; },
      pick);
}

// The divisor as one integer D = integer * 2^fracBits + fraction, so a fraction that would round
// up to 2^fracBits carries into the integer part.
constexpr Result<IntFrac> fromFixedPoint(std::uint64_t in,
                                         std::uint64_t want,
                                         Fixed         f,
                                         Pick          pick = Pick::nearest) {
    auto const unit = std::uint64_t{1} << f.fracBits;
    auto const lo   = mulChecked(f.intMin, unit);
    auto const hi   = mulChecked(f.intMax, unit) + (unit - 1);
    auto const n    = detail::neighbours(in, want, unit, f.scale, lo, hi);
    auto const r    = search(
      in,
      want,
      n,
      [&](std::uint64_t d) { return Rational{unit, mulChecked(d, f.scale)}; },
      pick);
    return {
      IntFrac{static_cast<std::uint32_t>(r.setting >> f.fracBits),
              static_cast<std::uint32_t>(r.setting & (unit - 1))},
      r.achieved,
      r.found
    };
}

// multiply by n / den, n first..last (the SAM SERCOM USART's arithmetic mode: f_ref x (65536 -
// BAUD) / 2^20)
struct Multiplier {
    std::uint32_t first;
    std::uint32_t last;
    std::uint64_t den;
};

// The two numerators around want x den / in, clamped; on a tie the smaller one wins.
constexpr auto fromMultiplier(std::uint64_t in,
                              std::uint64_t want,
                              Multiplier    m,
                              Pick          pick = Pick::nearest) {
    auto const                         n = detail::neighbours(want, in, m.den, 1, m.first, m.last);
    std::array<std::uint32_t, 2> const cs{static_cast<std::uint32_t>(n[0]),
                                          static_cast<std::uint32_t>(n[1])};
    return search(in, want, cs, [&](std::uint32_t k) { return Rational{k, m.den}; }, pick);
}

// ---- a rate known only inside a consteval function (a per-device clock) ------------------------
// A static_assert cannot see a function argument, so these are not constant expressions when the
// rate fails: the compiler's note "in call to 'rateOutOfTolerance(...)'" shows the numbers.

namespace detail {
    // not constexpr: reaching one is the compile error
    inline void rateOutsideItsToleranceSeeTheCallBelow() {}

    inline void rateOutOfReachSeeTheCallBelow() {}

    constexpr std::uint64_t milli(Rational a) {
        return (mulChecked(a.num, 1000) + a.den / 2) / a.den;
    }
}   // namespace detail

// achievedMilliHz: the achieved rate x 1000, rounded
constexpr void rateOutOfTolerance(std::uint64_t wantedHz,
                                  std::uint64_t achievedMilliHz,
                                  std::int64_t  errorPpm,
                                  std::uint64_t allowedPpm) {
    static_cast<void>(wantedHz);
    static_cast<void>(achievedMilliHz);
    static_cast<void>(errorPpm);
    static_cast<void>(allowedPpm);
    detail::rateOutsideItsToleranceSeeTheCallBelow();
}

// the range the hardware can reach, in mHz
constexpr void rateOutOfReach(std::uint64_t wantedHz,
                              std::uint64_t slowestMilliHz,
                              std::uint64_t fastestMilliHz) {
    static_cast<void>(wantedHz);
    static_cast<void>(slowestMilliHz);
    static_cast<void>(fastestMilliHz);
    detail::rateOutOfReachSeeTheCallBelow();
}

// inTolerance for a runtime argument of a consteval function: true, or not a constant expression
constexpr bool requireInTolerance(Rational      achieved,
                                  std::uint64_t wanted,
                                  Tolerance     tol) {
    if(!inTolerance(achieved, wanted, tol)) {
        rateOutOfTolerance(wanted,
                           detail::milli(achieved),
                           errorIn(achieved, wanted, 1'000'000),
                           (mulChecked(tol.num, 1'000'000) + tol.den / 2) / tol.den);
    }
    return true;
}

// ---- the message -------------------------------------------------------------------------------

template<std::size_t N>
struct FixedString {
    std::array<char, N> text{};

    consteval FixedString(char const (&s)[N]) {   // NOLINT(google-explicit-constructor)
        std::copy_n(s, N, text.begin());
    }

    constexpr std::string_view view() const { return {text.data(), N - 1}; }
};

// a string literal as a template argument deduces N (and keeps clang's -Wctad-maybe-unsupported
// quiet, which a firmware build treats as an error)
template<std::size_t N>
FixedString(char const (&)[N]) -> FixedString<N>;

namespace detail {
    struct Writer {
        std::array<char, 240> out{};
        std::size_t           n{};

        constexpr Writer& text(std::string_view s) {
            for(char c : s) {
                if(n < out.size()) { out[n++] = c; }
            }
            return *this;
        }

        constexpr Writer& integer(std::uint64_t v) {
            std::array<char, 24> b{};
            auto const           r = std::to_chars(b.data(), b.data() + b.size(), v);
            return text({b.data(), static_cast<std::size_t>(r.ptr - b.data())});
        }

        constexpr Writer& signedInteger(std::int64_t v) {
            if(v >= 0) { text("+"); }
            std::array<char, 24> b{};
            auto const           r = std::to_chars(b.data(), b.data() + b.size(), v);
            return text({b.data(), static_cast<std::size_t>(r.ptr - b.data())});
        }

        // num / den with one decimal, rounded to nearest
        constexpr Writer& fixed1(Rational a) {
            auto const tenths = (mulChecked(a.num, 10) + a.den / 2) / a.den;
            integer(tenths / 10);
            text(".");
            return integer(tenths % 10);
        }
    };
}   // namespace detail

template<Rational Achieved, std::uint64_t Wanted, Tolerance Tol, FixedString What>
struct ToleranceMessage {
    static constexpr detail::Writer storage = [] {
        detail::Writer w{};
        if(!What.view().empty()) { w.text(What.view()).text(": "); }
        w.text("wanted ").integer(Wanted).text(" Hz, got ").fixed1(Achieved).text(" Hz (");
        w.signedInteger(errorIn(Achieved, Wanted, 1'000'000)).text(" ppm), allowed ");
        w.integer((Tol.num * 1'000'000 + Tol.den / 2) / Tol.den).text(" ppm");
        return w;
    }();

    constexpr std::size_t size() const { return storage.n; }

    constexpr char const* data() const { return storage.out.data(); }

    constexpr std::string_view view() const { return {data(), size()}; }
};

template<Rational      Achieved,
         std::uint64_t Wanted,
         Tolerance     Tol,
         FixedString   What = "">
consteval void assertInTolerance() {
    static_assert(inTolerance(Achieved, Wanted, Tol),
                  ToleranceMessage<Achieved, Wanted, Tol, What>{});
}

}   // namespace Kvasir::Prescaler
