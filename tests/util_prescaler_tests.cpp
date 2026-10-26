// Kvasir::Prescaler: the front ends on hand-computed cases, ties, the PL011 carry, the message
// text, and the drivers' searches over grids of clocks and rates against references derived from
// the hardware's formulas in exact integers (no copy of any older driver code).
#include "kvasir/Util/Prescaler.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <tuple>

namespace P = Kvasir::Prescaler;

namespace {
// ---- front ends --------------------------------------------------------------------------------
constexpr auto r = P::fromRange(48'000'000, 7'000'000, {.first = 1, .last = 256});
static_assert(r.found && r.setting == 7
              && r.achieved
                   == P::Rational{48'000'000,
                                  7});
static_assert(r.errorPpm(7'000'000) == -20408);
// the same error in ppb: 1e6 * 1e9 / 49e6, rounded half up = 20'408'163 (48 MHz / 7 is below 7 MHz)
static_assert(r.errorPpb(7'000'000) == -20'408'163);

// a tie: 10 MHz / 3 and 10 MHz / 4 are equally far from 2.857142... - no, pick an exact tie:
// 12 MHz, want 5 MHz: /2 = 6 MHz (+1), /3 = 4 MHz (-1): the smaller divider wins
static_assert(P::fromRange(12'000'000,
                           5'000'000,
                           {.first = 1,
                            .last  = 10})
                .setting
              == 2);
// notAbove / notBelow
static_assert(P::fromRange(12'000'000,
                           5'000'000,
                           {.first = 1,
                            .last  = 10},
                           P::Pick::notAbove)
                .setting
              == 3);
static_assert(P::fromRange(12'000'000,
                           5'000'000,
                           {.first = 1,
                            .last  = 10},
                           P::Pick::notBelow)
                .setting
              == 2);
// clamped at the range's ends
static_assert(P::fromRange(12'000'000,
                           1,
                           {.first = 1,
                            .last  = 10})
                .setting
              == 10);
static_assert(P::fromRange(12'000'000,
                           100'000'000,
                           {.first = 1,
                            .last  = 10})
                .setting
              == 1);

constexpr std::array<std::uint32_t, 4> list{2, 8, 32, 128};
static_assert(P::fromList(1'000'000,
                          40'000,
                          list)
                .setting
              == 32);   // 31250 vs 125000
static_assert(P::fromPowerOfTwo(1'000'000,
                                40'000,
                                {.minExp = 0,
                                 .maxExp = 10})
                .setting
              == 5);

// the PL011 at 150 MHz, 115200: 150e6 / (16 * 115200) = 81.380...; 0.380 * 64 = 24.3 -> 24
constexpr P::Fixed pl011{.intMin = 1, .intMax = 65535, .fracBits = 6, .scale = 16};
static_assert(P::fromFixedPoint(150'000'000,
                                115'200,
                                pl011)
                .setting
              == P::IntFrac{81,
                            24});
// a divisor of x.995 carries into the integer: 48 MHz / (16 * 3747) = 800.62... no: pick a clock
// where the fraction rounds up to 64/64: 16 * 115200 * 3.998 = 7369121 Hz
static_assert(P::fromFixedPoint(7'369'121,
                                115'200,
                                pl011)
                .setting
              == P::IntFrac{4,
                            0});

// the generic search: two fields, PL022-like (cpsdvsr even 2..254, scr 0..255), not above
constexpr auto spi = P::search(
  125'000'000,
  7'000'000,
  std::array<std::array<std::uint32_t, 2>, 4>{
    {{2, 8}, {2, 9}, {4, 4}, {6, 2}}
},
  [](std::array<std::uint32_t, 2> c) { return P::Rational{1, std::uint64_t{c[0]} * (1 + c[1])}; },
  P::Pick::notAbove);
// 125/18 = 6.94, 125/20 = 6.25, 125/20, 125/18: first of the equal best wins
static_assert(spi.setting
              == std::array<std::uint32_t,
                            2>{2,
                               8});
static_assert(P::search(
                125'000'000,
                7'000'000,
                std::array<std::uint32_t,
                           3>{17,
                              18,
                              19},
                [](std::uint32_t d) { return P::Rational{1, d}; },
                P::Pick::firstFit)
                .setting
              == 18);

// ---- tolerance and the message -----------------------------------------------------------------
static_assert(P::inTolerance({117'187'5,
                              10},
                             115'200,
                             P::Tolerance{std::ratio<2,
                                                     100>{}}));
static_assert(!P::inTolerance({117'187'5,
                               10},
                              115'200,
                              P::Tolerance{std::ratio<5,
                                                      1000>{}}));
static_assert(P::inTolerance({100,
                              1},
                             100,
                             P::Tolerance{0,
                                          1}),
              "exact passes a zero tolerance");

constexpr std::string_view msg
  = Kvasir::Diagnostic::Message<P::ToleranceText<P::Rational{117'187'5, 10},
                                                 115'200,
                                                 P::Tolerance{std::ratio<5, 1000>{}},
                                                 "UART0 baud rate">>{}
      .view();
static_assert(
  msg == "UART0 baud rate: wanted 115200 Hz, got 117187.5 Hz (+17253 ppm), allowed 5000 ppm");

// ---- fromMultiplier: the SAM SERCOM USART, f_ref x (65536 - BAUD) / 2^20 ----------------------
constexpr P::Multiplier samArithmetic{.first = 1, .last = 65536, .den = std::uint64_t{1} << 20};

constexpr std::uint32_t samUsartBaud(std::uint32_t clk,
                                     std::uint32_t baud) {
    return 65536U - P::fromMultiplier(clk, baud, samArithmetic).setting;
}

// 48 MHz, 115200: 65536 (1 - 16 x 115200 / 48e6) = 63019.38 -> 63019
static_assert(samUsartBaud(48'000'000,
                           115'200)
              == 63019);
static_assert(P::fromMultiplier(48'000'000,
                                115'200,
                                samArithmetic)
                .achieved
              == P::Rational{48'000'000ULL * 2517,
                             std::uint64_t{1} << 20});
// a tie: 2^21 Hz, 3 Bd wants n = 1.5: the smaller n (the larger BAUD) wins, as round-half-up did
static_assert(samUsartBaud(2'097'152,
                           3)
              == 65535);
// clamped: faster than f_ref / 16 gives BAUD 0, slower than one step gives 65535
static_assert(samUsartBaud(1'000'000,
                           100'000)
              == 0);
static_assert(samUsartBaud(48'000'000,
                           1)
              == 65535);

// the SAM SERCOM SPI: 2 x (BAUD + 1), BAUD + 1 1..256, not above
constexpr P::Fixed samSck{.intMin = 1, .intMax = 256, .fracBits = 0, .scale = 2};

constexpr std::uint32_t samSpiBaud(std::uint32_t clk,
                                   std::uint32_t sck) {
    return P::fromFixedPoint(clk, sck, samSck, P::Pick::notAbove).setting.integer - 1U;
}

static_assert(samSpiBaud(48'000'000,
                         7'000'000)
              == 3);   // 48 / 8 = 6 MHz, not 48 / 6 = 8
static_assert(samSpiBaud(48'000'000,
                         24'000'000)
              == 0);
static_assert(samSpiBaud(48'000'000,
                         30'000'000)
              == 0);   // nothing not above: the nearest
static_assert(samSpiBaud(48'000'000,
                         1'000)
              == 255);   // clamped

// a rate known only inside a consteval function
static_assert(P::requireInTolerance({400'000,
                                     1},
                                    400'000,
                                    P::Tolerance{0,
                                                 1}));
static_assert(P::requireInTolerance({48'000'000,
                                     112},
                                    428'000,
                                    P::Tolerance{std::ratio<1,
                                                            100>{}}));

// ---- the drivers' searches -----------------------------------------------------------------------
// the PL022 adapter as the driver uses it (chip_rp_common/SPI.hpp): one index over the grid
// cpsdvsr even 2..254 (outer) x scr 0..255 (inner), not above, nearest otherwise
namespace Pl022 {
    struct Div {
        std::uint32_t scr, cpsdvsr;
    };

    constexpr Div decode(std::uint32_t i) { return {i % 256, 2 * (i / 256 + 1)}; }

    // the full grid, as the driver did it before (the reference for the reduced one)
    constexpr Div fullGrid(std::uint64_t clk,
                           std::uint64_t sck) {
        auto const r = P::search(
          clk,
          sck,
          std::views::iota(0U, 127U * 256U),
          [](std::uint32_t i) {
              auto const d = decode(i);
              return P::Rational{1, std::uint64_t{d.cpsdvsr} * (1 + d.scr)};
          },
          P::Pick::notAbove);
        return decode(r.setting);
    }

    // what chip_rp_common/SPI.hpp does now: two SCRs per CPSDVSR
    constexpr Div divider(std::uint64_t clk,
                          std::uint64_t sck) {
        auto const candidates
          = std::views::iota(0U, 127U * 2U) | std::views::transform([=](std::uint32_t i) {
                std::uint64_t const cps  = 2 * (std::uint64_t{i} / 2 + 1);
                std::uint64_t const den  = cps * sck;
                std::uint64_t const q    = clk / den;
                std::uint64_t const ceil = q + (clk % den != 0 ? 1 : 0);
                std::uint64_t const n    = i % 2 == 0 ? q : ceil;
                auto const          scr
                  = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(n, 1, 256) - 1);
                return (i / 2) * 256 + scr;
            });
        auto const r = P::search(
          clk,
          sck,
          candidates,
          [](std::uint32_t i) {
              auto const d = decode(i);
              return P::Rational{1, std::uint64_t{d.cpsdvsr} * (1 + d.scr)};
          },
          P::Pick::notAbove);
        return decode(r.setting);
    }
}   // namespace Pl022

constexpr std::array<std::uint32_t, 12> clocks{12'000'000,
                                               16'000'000,
                                               24'000'000,
                                               48'000'000,
                                               100'000'000,
                                               120'000'000,
                                               125'000'000,
                                               133'000'000,
                                               150'000'000,
                                               200'000'000,
                                               250'000'000,
                                               7'369'121};
constexpr std::array<std::uint32_t, 16> bauds{300,
                                              1200,
                                              2400,
                                              9600,
                                              19'200,
                                              38'400,
                                              57'600,
                                              74'880,
                                              115'200,
                                              230'400,
                                              250'000,
                                              460'800,
                                              921'600,
                                              1'000'000,
                                              1'500'000,
                                              3'000'000};

constexpr std::array<std::uint32_t, 14> sck{1'000,
                                            10'000,
                                            100'000,
                                            400'000,
                                            1'000'000,
                                            2'000'000,
                                            4'000'000,
                                            7'000'000,
                                            8'000'000,
                                            10'000'000,
                                            12'500'000,
                                            20'000'000,
                                            25'000'000,
                                            62'500'000};

// the SAM clocks: the D21/C21 at 1..48 MHz, the E5x up to 120 MHz, and odd ones
constexpr std::array<std::uint32_t, 14> samClocks{1'000'000,
                                                  4'000'000,
                                                  7'999'000,
                                                  8'000'000,
                                                  12'000'000,
                                                  16'000'000,
                                                  24'000'000,
                                                  32'000'000,
                                                  32'768,
                                                  46'875'000,
                                                  48'000'000,
                                                  60'000'000,
                                                  100'000'000,
                                                  120'000'000};

// ---- references: the hardware's formulas, in exact integers ----------------------------------------
// the tolerance's definition, cross-multiplied: |num / den - wanted| <= wanted x t.num / t.den
constexpr bool withinExact(P::Rational   a,
                           std::uint64_t wanted,
                           P::Tolerance  t) {
    std::uint64_t const w = wanted * a.den;
    std::uint64_t const d = a.num > w ? a.num - w : w - a.num;
    return d * t.den <= w * t.num;
}

// the PL011: the rate is clk / (16 x D / 64) = 4 clk / D for the 6-fraction-bit divisor D; the
// nearest rate comes from floor(4 clk / baud) (at or above baud) or the next D (below); a tie takes
// the smaller D
constexpr std::uint64_t pl011Divisor(std::uint64_t clk,
                                     std::uint64_t baud) {
    std::uint64_t const lo = 4 * clk / baud;
    std::uint64_t const hi = lo + 1;
    // (4 clk / lo - baud) <= (baud - 4 clk / hi), both sides times lo x hi
    return (4 * clk - baud * lo) * hi <= (baud * hi - 4 * clk) * lo ? lo : hi;
}

// the PL022: CPSDVSR (even, 2..254) x (SCR + 1) (1..256) divides clk. Not above: the smallest
// product p with p x sck >= clk - for each CPSDVSR the smallest SCR + 1 that reaches it -, the first
// CPSDVSR of equal products; none reaches it: the largest product (the slowest rate, the nearest)
constexpr Pl022::Div pl022Reference(std::uint64_t clk,
                                    std::uint64_t sck) {
    std::uint64_t const need = (clk + sck - 1) / sck;
    Pl022::Div          best{.scr = 255, .cpsdvsr = 254};
    std::uint64_t       bestProduct = 0;
    for(std::uint64_t cps = 2; cps <= 254; cps += 2) {
        std::uint64_t const n = std::max<std::uint64_t>(1, (need + cps - 1) / cps);
        if(n > 256 || (bestProduct != 0 && cps * n >= bestProduct)) { continue; }
        bestProduct = cps * n;
        best
          = {.scr = static_cast<std::uint32_t>(n - 1), .cpsdvsr = static_cast<std::uint32_t>(cps)};
    }
    return best;
}

// the SAM SERCOM USART (arithmetic mode): rate = clk / 16 x (1 - BAUD / 65536), linear in BAUD, so
// the nearest rate is BAUD = 65536 (1 - 16 baud / clk) rounded half up, clamped to 0..65535
constexpr std::uint32_t samUsartReference(std::uint64_t clk,
                                          std::uint64_t baud) {
    std::uint64_t const top = 65536 * clk;
    std::uint64_t const sub = 16 * 65536 * baud;
    if(sub >= top) { return 0; }
    std::uint64_t const x = (2 * (top - sub) + clk) / (2 * clk);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(x, 65535));
}

// the SAM SERCOM SPI: rate = clk / (2 (BAUD + 1)); not above: BAUD + 1 = ceil(clk / (2 sck)),
// clamped to 1..256 (1: nothing is above; 256: nothing reaches it, the slowest is the nearest)
constexpr std::uint32_t samSpiReference(std::uint64_t clk,
                                        std::uint64_t sck) {
    std::uint64_t const n = (clk + 2 * sck - 1) / (2 * sck);
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(n, 1, 256) - 1);
}

// spot values, computed once from these references
static_assert(pl011Divisor(125'000'000,
                           115'200)
              == 4340);   // {67, 52}: 115'207 Bd
static_assert(pl011Divisor(7'369'121,
                           115'200)
              == 256);   // the carry: {4, 0}
static_assert(samUsartReference(48'000'000,
                                115'200)
              == 63019);
static_assert(samSpiReference(48'000'000,
                              7'000'000)
              == 3);
static_assert(pl022Reference(125'000'000,
                             7'000'000)
                  .scr
                == 8
              && pl022Reference(125'000'000,
                                7'000'000)
                     .cpsdvsr
                   == 2);
}   // namespace

int main() {
    Kvasir::Test::test("PL011: the nearest divisor, the carry included");
    {
        int checked = 0;
        int differ  = 0;
        for(auto const clk : clocks) {
            for(auto const baud : bauds) {
                if(clk / (16 * baud) == 0) { continue; }   // "baudrate too high" in the driver
                auto const neu = P::fromFixedPoint(clk, baud, pl011).setting;
                auto const ref = pl011Divisor(clk, baud);
                ++checked;
                if(std::uint64_t{neu.integer} * 64 + neu.fraction != ref || neu.fraction > 63) {
                    ++differ;
                    std::printf("  pl011 differ: clk %u baud %u new {%u,%u} reference D %llu\n",
                                clk,
                                baud,
                                neu.integer,
                                neu.fraction,
                                static_cast<unsigned long long>(ref));
                }
            }
        }
        std::printf("  PL011: %d checked, %d differ\n", checked, differ);
        CHECK(differ == 0);
        CHECK(checked > 150);
    }

    Kvasir::Test::test("PL022: SPI.hpp's two SCRs per CPSDVSR == the full grid == the reference");
    {
        int differ = 0;
        for(auto const clk : clocks) {
            for(auto const sck : sck) {
                auto const neu  = Pl022::divider(clk, sck);
                auto const full = Pl022::fullGrid(clk, sck);
                auto const ref  = pl022Reference(clk, sck);
                if(neu.scr != ref.scr || neu.cpsdvsr != ref.cpsdvsr || full.scr != ref.scr
                   || full.cpsdvsr != ref.cpsdvsr)
                {
                    ++differ;
                    std::printf(
                      "  spi differ: clk %u sck %u new {scr %u, cps %u} full {scr %u, cps %u} "
                      "reference {scr %u, cps %u}\n",
                      clk,
                      sck,
                      neu.scr,
                      neu.cpsdvsr,
                      full.scr,
                      full.cpsdvsr,
                      ref.scr,
                      ref.cpsdvsr);
                }
            }
        }
        std::printf("  PL022: %d differ\n", differ);
        CHECK(differ == 0);
    }

    // a check verdict compared over these tolerances
    constexpr std::array<P::Tolerance, 4> tols{
      P::Tolerance{1, 10000},
      P::Tolerance{5,  1000},
      P::Tolerance{1,   100},
      P::Tolerance{1,     4}
    };
    int        verdicts      = 0;
    int        verdictDiffer = 0;
    auto const verdict       = [&](char const*   what,
                                   std::uint32_t clk,
                                   std::uint32_t rate,
                                   P::Rational   achieved,
                                   P::Tolerance  t) {
        ++verdicts;
        bool const n = P::inTolerance(achieved, rate, t);
        if(n != withinExact(achieved, rate, t)) {
            ++verdictDiffer;
            std::printf("  %s verdict: clk %u rate %u tol %llu/%llu inTolerance %d\n",
                        what,
                        clk,
                        rate,
                        static_cast<unsigned long long>(t.num),
                        static_cast<unsigned long long>(t.den),
                        static_cast<int>(n));
        }
    };

    Kvasir::Test::test(
      "SAM USART: BAUD == the reference, the achieved rate and its verdicts exact");
    {
        int differ = 0;
        for(auto const clk : samClocks) {
            for(auto const baud : bauds) {
                auto const neu      = samUsartBaud(clk, baud);
                auto const achieved = P::fromMultiplier(clk, baud, samArithmetic).achieved;
                // clk x (65536 - BAUD) / 2^20
                bool const rateOk = achieved.num * (std::uint64_t{1} << 20)
                                 == std::uint64_t{clk} * (65536U - neu) * achieved.den;
                if(neu != samUsartReference(clk, baud) || !rateOk) {
                    ++differ;
                    std::printf("  usart differ: clk %u baud %u new %u reference %u\n",
                                clk,
                                baud,
                                neu,
                                samUsartReference(clk, baud));
                }
                for(auto const t : tols) { verdict("usart", clk, baud, achieved, t); }
            }
        }
        std::printf("  SAM USART: %d differ\n", differ);
        CHECK(differ == 0);
    }

    Kvasir::Test::test("SAM SPI: BAUD == the reference, the achieved rate and its verdicts exact");
    {
        int differ = 0;
        for(auto const clk : samClocks) {
            for(auto const sck : sck) {
                auto const neu = samSpiBaud(clk, sck);
                auto const achieved
                  = P::fromFixedPoint(clk, sck, samSck, P::Pick::notAbove).achieved;
                bool const rateOk
                  = achieved.num * 2U * (neu + 1U) == std::uint64_t{clk} * achieved.den;
                if(neu != samSpiReference(clk, sck) || !rateOk) {
                    ++differ;
                    std::printf("  sam spi differ: clk %u sck %u new %u reference %u\n",
                                clk,
                                sck,
                                neu,
                                samSpiReference(clk, sck));
                }
                for(auto const t : tols) { verdict("sam spi", clk, sck, achieved, t); }
            }
        }
        std::printf("  SAM SPI: %d differ\n", differ);
        CHECK(differ == 0);
    }

    Kvasir::Test::test("SAM USART and SPI: == the references over 200000 random clock/rate pairs");
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        auto const    next  = [&] {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(state >> 33);
        };
        int differ = 0;
        for(int i = 0; i < 200'000; ++i) {
            std::uint32_t const clk  = 32'768 + next() % 120'000'000;
            std::uint32_t const rate = 1 + next() % (clk / 16);
            if(samUsartReference(clk, rate) != samUsartBaud(clk, rate)) {
                ++differ;
                std::printf("  usart differ: clk %u baud %u\n", clk, rate);
            }
            std::uint32_t const sck = 1 + next() % (clk / 2);
            if(samSpiReference(clk, sck) != samSpiBaud(clk, sck)) {
                ++differ;
                std::printf("  sam spi differ: clk %u sck %u\n", clk, sck);
            }
        }
        std::printf("  random pairs: %d differ\n", differ);
        CHECK(differ == 0);
    }

    Kvasir::Test::test(
      "SAM I2C: the SCL verdicts exact (f_GCLK / (10 + BAUD + BAUDLOW), / (2 + HS))");
    {
        constexpr std::array<std::uint32_t, 12> scl{10'000,
                                                    50'000,
                                                    100'000,
                                                    200'000,
                                                    250'000,
                                                    400'000,
                                                    500'000,
                                                    800'000,
                                                    1'000'000,
                                                    1'500'000,
                                                    2'000'000,
                                                    3'400'000};
        for(auto const clk : samClocks) {
            for(auto const scl : scl) {
                // every SCL period the registers can give: 10..520 (standard to Fm+), 2..512 (Hs)
                for(std::uint64_t period = 2; period <= 520; ++period) {
                    for(auto const t : tols) { verdict("sam i2c", clk, scl, {clk, period}, t); }
                }
            }
        }
    }

    Kvasir::Test::test("RP SPIQueued: the closest-not-above divider == the reference");
    {
        int        same  = 0;
        int        other = 0;
        auto const check = [&](std::uint32_t clk, std::uint32_t max) {
            auto const ref = pl022Reference(clk, max);
            auto const neu = Pl022::divider(clk, max);
            if(ref.scr == neu.scr && ref.cpsdvsr == neu.cpsdvsr) {
                ++same;
            } else {
                ++other;
                std::printf(
                  "  spiqueued differ: clk %u max %u reference {scr %u, cps %u} new {scr %u, "
                  "cps %u}\n",
                  clk,
                  max,
                  ref.scr,
                  ref.cpsdvsr,
                  neu.scr,
                  neu.cpsdvsr);
            }
        };
        std::uint64_t state = 0x2545F4914F6CDD1DULL;
        for(int i = 0; i < 3000; ++i) {
            state          = state * 6364136223846793005ULL + 1442695040888963407ULL;
            auto const clk = static_cast<std::uint32_t>(1'000'000 + (state >> 33) % 299'000'000);
            state          = state * 6364136223846793005ULL + 1442695040888963407ULL;
            auto const max = static_cast<std::uint32_t>(1 + (state >> 33) % (clk / 2 + 1000));
            check(clk, max);
        }
        std::printf("  RP SPIQueued: %d same, %d other\n", same, other);
        CHECK(other == 0);
    }

    Kvasir::Test::test("RP I2C: the SCL verdicts exact (period = clk / f rounded)");
    {
        constexpr std::array<std::uint32_t, 9>
          rates{10'000, 50'000, 100'000, 250'000, 400'000, 500'000, 800'000, 900'000, 1'000'000};
        for(auto const clk : clocks) {
            for(auto const f : rates) {
                std::uint64_t const period
                  = (std::uint64_t{clk} + f / 2) / f;   // HCNT + LCNT + the fixed cycles
                for(auto const t : tols) { verdict("rp i2c", clk, f, {clk, period}, t); }
            }
        }
    }

    Kvasir::Test::test("RP ClockOut: the verdicts exact (divider truncated to 1/256)");
    {
        for(auto const clk : clocks) {
            for(std::uint32_t out = 1'000; out <= clk / 2; out = out * 3 / 2 + 7) {
                std::uint64_t const div256
                  = std::uint64_t{clk} * 128U / out;   // clk / (2 out), x 256
                if(div256 < 256 || div256 >= std::uint64_t{65536U} * 256U) { continue; }
                verdict("clockout",
                        clk,
                        out,
                        {std::uint64_t{clk} * 256U, 2U * div256},
                        P::Tolerance{1, 1000});
            }
        }
    }
    std::printf("  inTolerance: %d verdicts, %d differ from the exact check\n",
                verdicts,
                verdictDiffer);
    CHECK(verdictDiffer == 0);

    return Kvasir::Test::report();
}
