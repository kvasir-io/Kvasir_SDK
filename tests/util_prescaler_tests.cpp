// Kvasir::Prescaler: the front ends on hand-computed cases, ties, the PL011 carry, the message
// text, and the old RP UART search against the new one over a grid of clocks and rates.
#include "kvasir/Util/Prescaler.hpp"
#include "support/prescaler_legacy.hpp"
#include "test_harness.hpp"
#if __has_include("PllSearch.hpp")
    #include "PllSearch.hpp"   // chip_rp_common, where it sits next to the SDK
    #define KVASIR_TEST_HAVE_RP_PLL 1
#endif

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
constexpr P::Fixed Pl011{.intMin = 1, .intMax = 65535, .fracBits = 6, .scale = 16};
static_assert(P::fromFixedPoint(150'000'000,
                                115'200,
                                Pl011)
                .setting
              == P::IntFrac{81,
                            24});
// a divisor of x.995 carries into the integer: 48 MHz / (16 * 3747) = 800.62... no: pick a clock
// where the fraction rounds up to 64/64: 16 * 115200 * 3.998 = 7369121 Hz
static_assert(P::fromFixedPoint(7'369'121,
                                115'200,
                                Pl011)
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
  [](std::array<std::uint32_t, 2> c) { return P::Rational{1, c[0] * (1 + c[1])}; },
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

constexpr std::string_view msg = P::ToleranceMessage<P::Rational{117'187'5, 10},
                                                     115'200,
                                                     P::Tolerance{std::ratio<5, 1000>{}},
                                                     "UART0 baud rate">{}
                                   .view();
static_assert(
  msg == "UART0 baud rate: wanted 115200 Hz, got 117187.5 Hz (+17253 ppm), allowed 5000 ppm");

// ---- fromMultiplier: the SAM SERCOM USART, f_ref x (65536 - BAUD) / 2^20 ----------------------
constexpr P::Multiplier SamArithmetic{.first = 1, .last = 65536, .den = std::uint64_t{1} << 20};

constexpr std::uint32_t samUsartBaud(std::uint32_t clk,
                                     std::uint32_t baud) {
    return 65536U - P::fromMultiplier(clk, baud, SamArithmetic).setting;
}

// 48 MHz, 115200: 65536 (1 - 16 x 115200 / 48e6) = 63019.38 -> 63019
static_assert(samUsartBaud(48'000'000,
                           115'200)
              == 63019);
static_assert(P::fromMultiplier(48'000'000,
                                115'200,
                                SamArithmetic)
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
constexpr P::Fixed SamSck{.intMin = 1, .intMax = 256, .fracBits = 0, .scale = 2};

constexpr std::uint32_t samSpiBaud(std::uint32_t clk,
                                   std::uint32_t sck) {
    return P::fromFixedPoint(clk, sck, SamSck, P::Pick::notAbove).setting.integer - 1U;
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

// the SAM SERCOM I2C host's SCL (Sercom_I2C.hpp achievedRate): f_GCLK / (10 + BAUD + BAUDLOW),
// (10 + 2 BAUD) with BAUDLOW 0, (2 + HSBAUD + HSBAUDLOW) / (2 + 2 HSBAUD) in high-speed
constexpr P::Rational samI2cAchieved(std::uint32_t            clk,
                                     Legacy::SamI2cBaudConfig c) {
    if(c.baud == 0 && c.baudlow == 0) {
        if(c.hsbaudlow == 0) { return {clk, 2U + 2U * std::uint64_t{c.hsbaud}}; }
        return {clk, 2U + std::uint64_t{c.hsbaud} + c.hsbaudlow};
    }
    if(c.hsbaud == 0 && c.hsbaudlow == 0) {
        if(c.baudlow == 0) { return {clk, 10U + 2U * std::uint64_t{c.baud}}; }
        return {clk, 10U + std::uint64_t{c.baud} + c.baudlow};
    }
    return {0, 1};
}

// ---- the old RP UART search against the new one --------------------------------------------------
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
                std::uint64_t const cps  = 2 * (i / 2 + 1);
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

constexpr std::array<std::uint32_t, 12> Clocks{12'000'000,
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
constexpr std::array<std::uint32_t, 16> Bauds{300,
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
}   // namespace

int main() {
    Kvasir::Test::test("PL011: old search == new search, except the carry");
    int same = 0, carry = 0, other = 0;
    for(auto clk : Clocks) {
        for(auto baud : Bauds) {
            if(clk / (16 * baud) == 0) { continue; }   // "baudrate too high" in the driver
            auto const old = Legacy::calcBaudRegs(clk, baud);
            auto const neu = P::fromFixedPoint(clk, baud, Pl011).setting;
            if(old.first == neu.integer && old.second == neu.fraction) {
                ++same;
            } else if(old.second == 64 && neu.integer == old.first + 1U && neu.fraction == 0) {
                ++carry;   // finding 1: the old search's {x, 64} fails to compile in the driver
            } else {
                ++other;
                std::printf("  differ: clk %u baud %u old {%u,%u} new {%u,%u}\n",
                            clk,
                            baud,
                            old.first,
                            old.second,
                            neu.integer,
                            neu.fraction);
            }
        }
    }
    std::printf("  %d same, %d carries (old {x,64}), %d other\n", same, carry, other);
    CHECK(other == 0);
    CHECK(same > 150);

    Kvasir::Test::test("PL022: old search == new search");
    int                                     spiSame = 0, spiOther = 0;
    constexpr std::array<std::uint32_t, 14> Sck{1'000,
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
    for(auto clk : Clocks) {
        for(auto sck : Sck) {
            auto const old  = Legacy::spiCalcBaudRegs(clk, sck);
            auto const neu  = Pl022::divider(clk, sck);
            auto const full = Pl022::fullGrid(clk, sck);
            if(old.first == neu.scr && old.second == neu.cpsdvsr && full.scr == neu.scr
               && full.cpsdvsr == neu.cpsdvsr)
            {
                ++spiSame;
            } else {
                ++spiOther;
                std::printf(
                  "  spi differ: clk %u sck %u old {scr %u, cps %u} new {scr %u, cps %u}\n",
                  clk,
                  sck,
                  old.first,
                  old.second,
                  neu.scr,
                  neu.cpsdvsr);
            }
        }
    }
    std::printf("  PL022: %d same, %d other\n", spiSame, spiOther);
    CHECK(spiOther == 0);

    // the SAM clocks: the D21/C21 at 1..48 MHz, the E5x up to 120 MHz, and odd ones
    constexpr std::array<std::uint32_t, 14> SamClocks{1'000'000,
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
    // a check verdict compared over these tolerances
    constexpr std::array<P::Tolerance, 4> Tols{
      P::Tolerance{1, 10000},
      P::Tolerance{5,  1000},
      P::Tolerance{1,   100},
      P::Tolerance{1,     4}
    };

    Kvasir::Test::test("SAM USART: old BAUD == new BAUD, old check == exact check");
    int usartSame = 0, usartOther = 0, usartVerdict = 0;
    for(auto clk : SamClocks) {
        for(auto baud : Bauds) {
            auto const old = Legacy::samUsartCalcBaudReg(clk, baud);
            auto const neu = samUsartBaud(clk, baud);
            if(old == neu) {
                ++usartSame;
            } else {
                ++usartOther;
                std::printf("  usart differ: clk %u baud %u old %u new %u\n", clk, baud, old, neu);
            }
            auto const achieved = P::fromMultiplier(clk, baud, SamArithmetic).achieved;
            for(auto t : Tols) {
                bool const o = Legacy::samUsartIsValid(clk, baud, double(t.num), double(t.den));
                bool const n = P::inTolerance(achieved, baud, t);
                if(o != n) {
                    ++usartVerdict;
                    std::printf("  usart verdict: clk %u baud %u tol %llu/%llu old %d new %d\n",
                                clk,
                                baud,
                                static_cast<unsigned long long>(t.num),
                                static_cast<unsigned long long>(t.den),
                                o,
                                n);
                }
            }
        }
    }
    std::printf("  SAM USART: %d same, %d other, %d verdicts differ\n",
                usartSame,
                usartOther,
                usartVerdict);
    CHECK(usartOther == 0);
    CHECK(usartVerdict == 0);

    Kvasir::Test::test("SAM SPI: old BAUD == new BAUD, old check == exact check");
    int samSpiSame = 0, samSpiOther = 0, samSpiVerdict = 0;
    for(auto clk : SamClocks) {
        for(auto sck : Sck) {
            auto const old = Legacy::samSpiCalcBaudReg(clk, sck);
            auto const neu = samSpiBaud(clk, sck);
            if(old == neu) {
                ++samSpiSame;
            } else {
                ++samSpiOther;
                std::printf("  sam spi differ: clk %u sck %u old %u new %u\n", clk, sck, old, neu);
            }
            auto const achieved = P::fromFixedPoint(clk, sck, SamSck, P::Pick::notAbove).achieved;
            for(auto t : Tols) {
                bool const o = Legacy::samSpiIsValid(clk, sck, double(t.num), double(t.den));
                bool const n = P::inTolerance(achieved, sck, t);
                if(o != n) {
                    ++samSpiVerdict;
                    std::printf("  sam spi verdict: clk %u sck %u tol %llu/%llu old %d new %d\n",
                                clk,
                                sck,
                                static_cast<unsigned long long>(t.num),
                                static_cast<unsigned long long>(t.den),
                                o,
                                n);
                }
            }
        }
    }
    std::printf("  SAM SPI: %d same, %d other, %d verdicts differ\n",
                samSpiSame,
                samSpiOther,
                samSpiVerdict);
    CHECK(samSpiOther == 0);
    CHECK(samSpiVerdict == 0);

    Kvasir::Test::test("SAM USART and SPI: old == new over 200000 random clock/rate pairs");
    {
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        auto          next  = [&] {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<std::uint32_t>(state >> 33);
        };
        int differ = 0;
        for(int i = 0; i < 200'000; ++i) {
            std::uint32_t const clk  = 32'768 + next() % 120'000'000;
            std::uint32_t const rate = 1 + next() % (clk / 16);
            if(Legacy::samUsartCalcBaudReg(clk, rate) != samUsartBaud(clk, rate)) {
                ++differ;
                std::printf("  usart differ: clk %u baud %u\n", clk, rate);
            }
            std::uint32_t const sck = 1 + next() % (clk / 2);
            if(Legacy::samSpiCalcBaudReg(clk, sck) != samSpiBaud(clk, sck)) {
                ++differ;
                std::printf("  sam spi differ: clk %u sck %u\n", clk, sck);
            }
        }
        std::printf("  random pairs: %d differ\n", differ);
        CHECK(differ == 0);
    }

    Kvasir::Test::test("SAM I2C: old check == exact check");
    constexpr std::array<std::uint32_t, 12> Scl{10'000,
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
    int                                     i2cChecked = 0, i2cVerdict = 0;
    for(auto clk : SamClocks) {
        for(auto scl : Scl) {
            auto const cfg      = Legacy::samI2cCalcBaudConfig(clk, scl);
            auto const achieved = samI2cAchieved(clk, cfg);
            for(auto t : Tols) {
                ++i2cChecked;
                bool const o = Legacy::samI2cIsValid(clk, scl, double(t.num), double(t.den));
                bool const n = P::inTolerance(achieved, scl, t);
                if(o != n) {
                    ++i2cVerdict;
                    std::printf("  sam i2c verdict: clk %u scl %u tol %llu/%llu old %d new %d\n",
                                clk,
                                scl,
                                static_cast<unsigned long long>(t.num),
                                static_cast<unsigned long long>(t.den),
                                o,
                                n);
                }
            }
        }
    }
    std::printf("  SAM I2C: %d verdicts checked, %d differ\n", i2cChecked, i2cVerdict);
    CHECK(i2cVerdict == 0);

    Kvasir::Test::test("RP SPIQueued: the closest-not-above loop == SPI.hpp's search");
    {
        int  same = 0, other = 0;
        auto check = [&](std::uint32_t clk, std::uint32_t max) {
            if(std::uint64_t{max} * 254U * 256U < clk) { return; }   // rateOutOfReach
            auto const old = Legacy::rpSpiQueuedSetup(clk, max);
            auto const neu = Pl022::divider(clk, max);
            if(old.first == neu.scr && old.second == neu.cpsdvsr) {
                ++same;
            } else {
                ++other;
                std::printf(
                  "  spiqueued differ: clk %u max %u old {scr %u, cps %u} new {scr %u, "
                  "cps %u}\n",
                  clk,
                  max,
                  old.first,
                  old.second,
                  neu.scr,
                  neu.cpsdvsr);
            }
        };
        for(auto clk : Clocks) {
            for(auto sck : Sck) { check(clk, sck); }
        }
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

    Kvasir::Test::test("RP I2C: old truncating check vs exact check (counts unchanged)");
    {
        // the 50 ns spike filter and the 60/40 split make HCNT/LCNT invalid below a few MHz
        constexpr std::array<std::uint32_t, 9>
            Rates{10'000, 50'000, 100'000, 250'000, 400'000, 500'000, 800'000, 900'000, 1'000'000};
        int checked = 0, looser = 0, stricter = 0;
        for(auto clk : Clocks) {
            for(auto f : Rates) {
                auto const regs = Legacy::rpI2cCalcBaudRegs(clk, f);
                if(regs.hcnt > 0xFFFF || regs.hcnt <= regs.spklen + 5 || regs.lcnt > 0xFFFF
                   || regs.lcnt <= regs.spklen + 7)
                {
                    continue;   // refused by the HCNT/LCNT asserts before the rate check
                }
                P::Rational const achieved{clk,
                                           std::uint64_t{regs.hcnt} + regs.spklen + 7 + regs.lcnt
                                             + 1};
                for(auto t : Tols) {
                    ++checked;
                    bool const o = Legacy::rpI2cIsValid(clk, f, t.num, t.den);
                    bool const n = P::inTolerance(achieved, f, t);
                    if(o == n) { continue; }
                    (n ? looser : stricter)++;
                    std::printf(
                      "  rp i2c verdict: clk %u scl %u tol %llu/%llu old %d new %d "
                      "(achieved %.3f Hz)\n",
                      clk,
                      f,
                      static_cast<unsigned long long>(t.num),
                      static_cast<unsigned long long>(t.den),
                      o,
                      n,
                      double(achieved.num) / double(achieved.den));
                }
            }
        }
        // finding 5 of the plan: the old check truncated the achieved rate and the allowance to
        // whole Hz, so it could only differ by less than 2 Hz, in either direction
        std::printf("  RP I2C: %d verdicts, %d now pass that failed, %d now fail that passed\n",
                    checked,
                    looser,
                    stricter);
        CHECK(stricter == 0);
    }

    Kvasir::Test::test("RP ClockOut: old double check vs exact check");
    {
        int checked = 0, differ = 0;
        for(auto clk : Clocks) {
            for(std::uint32_t out = 1'000; out <= clk / 2; out = out * 3 / 2 + 7) {
                double const div = double(clk) / (2.0 * double(out));
                if(div < 1.0 || div >= 65536.0) { continue; }
                auto const        di = static_cast<std::uint16_t>(div);
                auto const        df = static_cast<std::uint8_t>((div - di) * 256.0);
                P::Rational const achieved{std::uint64_t{clk} * 256U,
                                           2U * (std::uint64_t{di} * 256U + df)};
                ++checked;
                bool const o = Legacy::rpClockOutIsValid(clk, out);
                bool const n = P::inTolerance(achieved, out, P::Tolerance{1, 1000});
                if(o != n) {
                    ++differ;
                    std::printf("  clockout verdict: clk %u out %u old %d new %d\n",
                                clk,
                                out,
                                o,
                                n);
                }
            }
        }
        std::printf("  RP ClockOut: %d verdicts, %d differ\n", checked, differ);
        CHECK(differ == 0);
    }

#ifdef KVASIR_TEST_HAVE_RP_PLL
    Kvasir::Test::test("RP PLL: vcocalc.py's double search == the exact one");
    {
        namespace D = Kvasir::DefaultClockSettings::detail;
        constexpr std::array<std::uint32_t, 10> Crystals{12'000'000,
                                                         8'000'000,
                                                         10'000'000,
                                                         16'000'000,
                                                         20'000'000,
                                                         24'000'000,
                                                         25'000'000,
                                                         30'000'000,
                                                         48'000'000,
                                                         7'372'800};
        std::array<std::uint32_t, 4> const odd{133'333'333, 99'999'999, 125'000'001, 48'000'000};
        int                                same = 0, other = 0, noise = 0;
        auto check = [&](std::uint32_t crystal, std::uint32_t target) {
            auto const o  = Legacy::calcPllSettings<false>(target, crystal);
            auto const n  = D::calcPllSettings<false>(target, crystal);
            auto const ol = Legacy::calcPllSettings<true>(target, crystal);
            auto const nl = D::calcPllSettings<true>(target, crystal);
            auto const eq = [](auto a, D::PllSettings b) {
                return a.fbdiv == b.fbdiv && a.pd1 == b.pd1 && a.pd2 == b.pd2
                    && a.refdiv == b.refdiv;
            };
            for(auto [a, b, low] :
                {
                  std::tuple{ o,  n, false},
                  std::tuple{ol, nl,  true}
            })
            {
                if(eq(a, b)) {
                    ++same;
                    continue;
                }
                // Expected: two settings with exactly the same output, which the double search
                // saw as different margins (a few ulp, above its 1e-9 Hz) where the exact one sees
                // a tie and takes the VCO vcocalc.py's rule prefers. Only where FREF / REFDIV is no
                // whole number of Hz (48 MHz / 7), so never with a 12 MHz crystal.
                D::PllSettings const old{a.fbdiv, a.pd1, a.pd2, a.refdiv};
                auto const           vcoBetter
                  = low ? std::uint64_t{b.fbdiv} * a.refdiv < std::uint64_t{a.fbdiv} * b.refdiv
                        : std::uint64_t{b.fbdiv} * a.refdiv > std::uint64_t{a.fbdiv} * b.refdiv;
                auto const ao = D::pllOutput(crystal, old);
                auto const bo = D::pllOutput(crystal, b);
                if(a.refdiv != 0 && crystal % a.refdiv != 0 && ao.num * bo.den == bo.num * ao.den
                   && vcoBetter)
                {
                    ++noise;
                    std::printf(
                      "  pll tie (expected): crystal %u target %u%s old {ref %u fb %u pd1 "
                      "%u pd2 %u} new {ref %u fb %u pd1 %u pd2 %u}\n",
                      crystal,
                      target,
                      low ? " lowvco" : "",
                      a.refdiv,
                      a.fbdiv,
                      a.pd1,
                      a.pd2,
                      b.refdiv,
                      b.fbdiv,
                      b.pd1,
                      b.pd2);
                    continue;
                }
                ++other;
                std::printf(
                  "  pll differ: crystal %u target %u%s old {ref %u fb %u pd1 %u pd2 %u} "
                  "new {ref %u fb %u pd1 %u pd2 %u}\n",
                  crystal,
                  target,
                  low ? " lowvco" : "",
                  a.refdiv,
                  a.fbdiv,
                  a.pd1,
                  a.pd2,
                  b.refdiv,
                  b.fbdiv,
                  b.pd1,
                  b.pd2);
            }
        };
        for(auto crystal : Crystals) {
            // every MHz with the 12 MHz crystal every RP board here has, every 5 MHz otherwise
            std::uint32_t const step = crystal == 12'000'000 ? 1'000'000 : 5'000'000;
            for(std::uint32_t target = 10'000'000; target <= 300'000'000; target += step) {
                check(crystal, target);
            }
            for(auto t : odd) { check(crystal, t); }
            for(auto t : Clocks) { check(crystal, t); }
        }
        std::uint64_t state = 0xD1B54A32D192ED03ULL;
        for(int i = 0; i < 400; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            auto const target
              = static_cast<std::uint32_t>(16'000'000 + (state >> 33) % 284'000'000);
            check(Crystals[static_cast<std::size_t>(i) % Crystals.size()], target);
        }
        std::printf("  RP PLL: %d same, %d exact ties the double search missed, %d other\n",
                    same,
                    noise,
                    other);
        CHECK(other == 0);
    }
#endif
    return Kvasir::Test::report();
}
