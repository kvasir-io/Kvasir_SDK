#pragma once
// The hand-written searches the drivers had before Kvasir/Util/Prescaler.hpp, copied verbatim, so
// util_prescaler_tests can check the new helper gives the same register values over a grid.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace Legacy {
// chip_rp_common/UART.hpp:53-79 (before the migration)
static constexpr double calcf_Baud(std::uint32_t f_clockSpeed,
                                   std::uint32_t divint,
                                   std::uint32_t divfrac) {
    return (double(f_clockSpeed) / (16.0 * (double(divint) + double(divfrac) / 64.0)));
}

static constexpr std::pair<std::uint16_t,
                           std::uint8_t>
calcBaudRegs(std::uint32_t f_clockSpeed,
             std::uint32_t f_baud) {
    std::pair<std::uint16_t, std::uint8_t> ret{};
    double                                 best = std::numeric_limits<double>::max();

    std::uint32_t divint = f_clockSpeed / (16 * f_baud);
    ret.first            = static_cast<std::uint16_t>(divint);

    for(std::uint32_t divfrac = 0; divfrac < 65; ++divfrac) {
        double f_div     = f_baud - calcf_Baud(f_clockSpeed, divint, divfrac);
        double abs_f_div = f_div > 0.0 ? f_div : -f_div;
        if(best > abs_f_div) {
            best       = abs_f_div;
            ret.second = static_cast<std::uint8_t>(divfrac);
            if(abs_f_div == 0.0) { return ret; }
        }
    }
    return ret;
}

// chip_rp_common/SPI.hpp:185-224 (before the migration): pair{scr, cpsdvsr}
static constexpr double spiCalcfBaud(std::uint32_t f_clockSpeed,
                                     std::uint32_t scr,
                                     std::uint32_t cpsdvsr) {
    return (double(f_clockSpeed) / (double(cpsdvsr) * (1.0 + double(scr))));
}

static constexpr std::pair<std::uint8_t,
                           std::uint8_t>
spiCalcBaudRegs(std::uint32_t f_clockSpeed,
                std::uint32_t f_baud) {
    std::pair<std::uint8_t, std::uint8_t> ret{};
    std::pair<std::uint8_t, std::uint8_t> retClosest{};
    double                                bestUnder = std::numeric_limits<double>::max();
    double                                bestAbs   = std::numeric_limits<double>::max();
    bool                                  haveUnder = false;

    for(std::uint32_t cpsdvsr = 2; cpsdvsr < 255; cpsdvsr += 2) {
        for(std::uint32_t scr = 0; scr < 256; ++scr) {
            double f_div     = f_baud - spiCalcfBaud(f_clockSpeed, scr, cpsdvsr);
            double abs_f_div = f_div > 0.0 ? f_div : -f_div;

            if(bestAbs > abs_f_div) {
                bestAbs           = abs_f_div;
                retClosest.first  = static_cast<std::uint8_t>(scr);
                retClosest.second = static_cast<std::uint8_t>(cpsdvsr);
            }
            if(f_div >= 0.0 && bestUnder > f_div) {
                haveUnder  = true;
                bestUnder  = f_div;
                ret.first  = static_cast<std::uint8_t>(scr);
                ret.second = static_cast<std::uint8_t>(cpsdvsr);
                if(f_div == 0.0) { return ret; }
            }
        }
    }
    return haveUnder ? ret : retClosest;
}

// chip_atsam_common/Sercom_Usart.hpp:229-253 (before the migration); the template's
// check as a plain function of its arguments
constexpr std::uint32_t samUsartCalcBaudReg(std::uint32_t f_clockSpeed,
                                            std::uint32_t f_baud) {
    auto baudReg = std::int64_t((65536.0   //NOLINT(bugprone-incorrect-roundings)
                                 * (1.0 - 16.0 * (double(f_baud) / double(f_clockSpeed))))
                                + 0.5);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(baudReg, 0, 65535));
}

constexpr double samUsartCalcfBaud(std::uint32_t f_clockSpeed,
                                   std::uint32_t baudReg) {
    return (double(f_clockSpeed) / 16.0) * (1.0 - (double(baudReg) / 65536.0));
}

constexpr bool samUsartIsValid(std::uint32_t f_clockSpeed,
                               std::uint32_t f_baud,
                               double        num,
                               double        denom) {
    auto const baudReg      = samUsartCalcBaudReg(f_clockSpeed, f_baud);
    auto const f_baudCalced = samUsartCalcfBaud(f_clockSpeed, baudReg);
    auto const err          = f_baudCalced - double(f_baud);
    auto const absErr       = err > 0.0 ? err : -err;
    return absErr <= (double(f_baud) * (num / denom));
}

// chip_atsam_common/Sercom_SPI.hpp:251-259 just before the move to Prescaler.hpp (BAUD + 1
// rounded up, clamped to 1..256), before the migration
constexpr std::uint32_t samSpiCalcBaudReg(std::uint32_t f_clockSpeed,
                                          std::uint32_t f_baud) {
    auto const div = (std::uint64_t{f_clockSpeed} + 2U * std::uint64_t{f_baud} - 1U)
                   / (2U * std::uint64_t{f_baud});   // BAUD + 1
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(div, 1, 256) - 1U);
}

constexpr double samSpiCalcfBaud(std::uint32_t f_clockSpeed,
                                 std::uint32_t baudReg) {
    return (double(f_clockSpeed) / (2.0 * (double(baudReg) + 1.0)));
}

constexpr bool samSpiIsValid(std::uint32_t f_clockSpeed,
                             std::uint32_t f_baud,
                             double        num,
                             double        denom) {
    auto const baudReg      = samSpiCalcBaudReg(f_clockSpeed, f_baud);
    auto const f_baudCalced = samSpiCalcfBaud(f_clockSpeed, baudReg);
    auto const err          = f_baudCalced - double(f_baud);
    auto const absErr       = err > 0.0 ? err : -err;
    return absErr <= (double(f_baud) * (num / denom));
}

// chip_atsam_common/Sercom_I2C.hpp:58-145 (before the migration): the registers stay
// as they were, only the check moved
struct SamI2cBaudConfig {
    unsigned char baud;
    unsigned char baudlow;
    unsigned char hsbaud;
    unsigned char hsbaudlow;
};

struct SamI2cBaudConfigRaw {
    unsigned char baud;
    unsigned char baudlow;
};

inline constexpr unsigned samI2cMaxSpeedStandard = 100'000;
inline constexpr unsigned samI2cMaxSpeedFast     = 400'000;
inline constexpr unsigned samI2cMaxSpeedFastPlus = 1'000'000;

constexpr SamI2cBaudConfigRaw samI2cCalcBaudConfigRaw(std::uint32_t f_clockSpeed,
                                                      std::uint32_t f_baud,
                                                      double        val,
                                                      double        lowtime,
                                                      double        hightime) {
    double     baudraw = ((double(f_clockSpeed) / double(f_baud)) - val);
    auto const baud    = std::int64_t(
      (baudraw * (hightime / (lowtime + hightime)))   //NOLINT(bugprone-incorrect-roundings)
      + 0.5);
    auto const baudlow    = std::int64_t((baudraw * (lowtime / (lowtime + hightime)))
                                         + 0.5);   //NOLINT(bugprone-incorrect-roundings)
    auto const baudReg    = static_cast<unsigned char>(std::clamp<std::int64_t>(baud, 0, 255));
    auto const baudlowReg = static_cast<unsigned char>(std::clamp<std::int64_t>(baudlow, 0, 255));
    return {baudReg, baudlowReg};
}

constexpr SamI2cBaudConfig samI2cCalcBaudConfig(std::uint32_t f_clockSpeed,
                                                std::uint32_t f_baud) {
    if(f_baud <= samI2cMaxSpeedStandard) {
        auto const raw = samI2cCalcBaudConfigRaw(f_clockSpeed, f_baud, 10, 4.7, 4.0);
        return {raw.baud, raw.baudlow, 0, 0};
    }
    if(f_baud <= samI2cMaxSpeedFast) {
        auto const raw = samI2cCalcBaudConfigRaw(f_clockSpeed, f_baud, 10, 1.3, 0.6);
        return {raw.baud, raw.baudlow, 0, 0};
    }
    if(f_baud <= samI2cMaxSpeedFastPlus) {
        auto const raw = samI2cCalcBaudConfigRaw(f_clockSpeed, f_baud, 10, 0.5, 0.26);
        return {raw.baud, raw.baudlow, 0, 0};
    }
    auto const raw = samI2cCalcBaudConfigRaw(f_clockSpeed, f_baud, 2, 2.0, 1.0);
    return {0, 0, raw.baud, raw.baudlow};
}

constexpr double samI2cCalcfBaud(std::uint32_t    f_clockSpeed,
                                 SamI2cBaudConfig baudConfig) {
    if(baudConfig.baud == 0 && baudConfig.baudlow == 0) {
        // high_speed
        if(baudConfig.hsbaudlow == 0) {
            return double(f_clockSpeed) / (2.0 + 2.0 * double(baudConfig.hsbaud));
        }
        return double(f_clockSpeed)
             / (2.0 + double(baudConfig.hsbaud) + double(baudConfig.hsbaudlow));
    }
    if(baudConfig.hsbaud == 0 && baudConfig.hsbaudlow == 0) {
        // other
        if(baudConfig.baudlow == 0) {
            return double(f_clockSpeed) / (10.0 + 2.0 * double(baudConfig.baud));
        }
        return double(f_clockSpeed) / (10.0 + double(baudConfig.baud) + double(baudConfig.baudlow));
    }
    return std::numeric_limits<double>::min();
}

constexpr bool samI2cIsValid(std::uint32_t f_clockSpeed,
                             std::uint32_t f_baud,
                             double        num,
                             double        denom) {
    auto const baudConfig   = samI2cCalcBaudConfig(f_clockSpeed, f_baud);
    auto const f_baudCalced = samI2cCalcfBaud(f_clockSpeed, baudConfig);
    auto const err          = f_baudCalced - double(f_baud);
    auto const absErr       = err > 0.0 ? err : -err;
    return absErr <= (double(f_baud) * (num / denom));
}

// chip_rp_common/clock_config.hpp:39-119 (before the migration)
struct PllSettings {
    std::uint32_t fbdiv;
    std::uint32_t pd1;
    std::uint32_t pd2;
    std::uint32_t refdiv;
};

//Use a lower VCO frequency when possible. This reduces power consumption, at the cost of increased jitter"
template<bool LowVco = false>
static constexpr PllSettings calcPllSettings(double clockSpeed,
                                             double crystalSpeed) {
    // RP2350 PLL constraints from datasheet
    constexpr double vco_max = 1'600'000'000;
    constexpr double vco_min = 750'000'000;
    constexpr double ref_min = 5'000'000;

    constexpr std::uint32_t fbdiv_max   = 320;
    constexpr std::uint32_t fbdiv_min   = 16;
    constexpr std::uint32_t refdiv_min  = 1;
    constexpr std::uint32_t refdiv_max  = 63;
    constexpr std::uint32_t postdiv_max = 7;
    constexpr std::uint32_t postdiv_min = 1;

    // Calculate REFDIV range based on minimum reference frequency constraint
    auto refdiv_range_max = static_cast<std::uint32_t>(crystalSpeed / ref_min);
    refdiv_range_max      = std::min(refdiv_range_max, refdiv_max);
    refdiv_range_max      = std::max(refdiv_range_max, refdiv_min);

    PllSettings bestSettings{.fbdiv = 0, .pd1 = 0, .pd2 = 0, .refdiv = 0};
    double      bestMargin = clockSpeed;
    double      bestVco    = 0.0;

    // Search algorithm matching RP2350 vcocalc.py
    for(std::uint32_t refdiv = refdiv_min; refdiv <= refdiv_range_max; ++refdiv) {
        double const refFreq = crystalSpeed / refdiv;
        if(refFreq < ref_min) {
            continue;   // Skip if reference frequency too low
        }

        for(std::uint32_t fbdiv = fbdiv_min; fbdiv <= fbdiv_max; ++fbdiv) {
            double const vco = refFreq * fbdiv;
            if(vco < vco_min || vco > vco_max) { continue; }

            // pd1 is inner loop to prefer higher pd1:pd2 ratios for lower power
            for(std::uint32_t pd2 = postdiv_min; pd2 <= postdiv_max; ++pd2) {
                for(std::uint32_t pd1 = postdiv_min; pd1 <= postdiv_max; ++pd1) {
                    // Check for integer frequency ratios (from vcocalc.py line 50)
                    if(static_cast<std::uint64_t>(vco * 1000)
                         % static_cast<std::uint64_t>(pd1 * pd2)
                       != 0)
                    {
                        continue;
                    }

                    double const out    = vco / pd1 / pd2;
                    double const margin = out > clockSpeed ? out - clockSpeed : clockSpeed - out;

                    // VCO preference logic from vcocalc.py line 49
                    bool const vcoIsBetter = LowVco ? (vco < bestVco) : (vco > bestVco);

                    // Accept if better margin, or same margin with preferred VCO
                    constexpr double tolerance = 1e-9;
                    bool const       marginEqual
                      = (margin - bestMargin) < tolerance && (margin - bestMargin) > -tolerance;

                    if(margin < bestMargin || (marginEqual && vcoIsBetter)) {
                        bestSettings
                          = PllSettings{.fbdiv = fbdiv, .pd1 = pd1, .pd2 = pd2, .refdiv = refdiv};
                        bestMargin = margin;
                        bestVco    = vco;
                    }
                }
            }
        }
    }

    return bestSettings;
}

// chip_rp_common/SPIQueued.hpp:65-97 just before the move to Prescaler.hpp (the smallest
// CPSDVSR x (SCR + 1) not below ceil(clk / max)), before the migration: pair{scr, cpsdvsr}
constexpr std::pair<std::uint32_t,
                    std::uint32_t>
rpSpiQueuedSetup(std::uint32_t clk,
                 std::uint32_t max) {
    std::uint32_t div = (clk + max - 1U) / max;
    if(div < 2U) { div = 2U; }
    std::uint32_t bestCps = 0;
    std::uint32_t bestScr = 0;
    for(std::uint32_t cps = 2; cps <= 254; cps += 2) {
        std::uint32_t const scr1 = (div + cps - 1U) / cps;
        if(scr1 >= 1U && scr1 <= 256U && (bestCps == 0U || cps * scr1 < bestCps * bestScr)) {
            bestCps = cps;
            bestScr = scr1;
        }
    }
    return {bestScr - 1U, bestCps};
}

// chip_rp_common/I2C.hpp:75-149 (before the migration): the counts, and the old
// truncating integer check (the counts stay, only the check moved)
struct RpI2cRegs {
    std::uint32_t hcnt;
    std::uint32_t lcnt;
    std::uint32_t spklen;
};

constexpr RpI2cRegs rpI2cCalcBaudRegs(std::uint32_t f_clockSpeed,
                                      std::uint32_t f_baud) {
    std::uint32_t const period      = (f_clockSpeed + f_baud / 2) / f_baud;
    std::uint32_t const high_cycles = period - (period * 3 / 5);
    std::uint32_t const low_cycles  = period * 3 / 5;
    auto const cycles = static_cast<std::uint32_t>((std::uint64_t{f_clockSpeed} * 50 + 999'999'999)
                                                   / 1'000'000'000);
    std::uint32_t const spklen = cycles == 0 ? 1U : cycles;
    return {high_cycles - spklen - 7, low_cycles - 1, spklen};
}

constexpr bool rpI2cIsValid(std::uint32_t f_clockSpeed,
                            std::uint32_t f_baud,
                            std::uint64_t Num,
                            std::uint64_t Denom) {
    auto const regs         = rpI2cCalcBaudRegs(f_clockSpeed, f_baud);
    auto const period       = (regs.hcnt + regs.spklen + 7) + (regs.lcnt + 1);
    auto const f_baudActual = f_clockSpeed / period;
    auto const err          = f_baudActual > f_baud ? f_baudActual - f_baud : f_baud - f_baudActual;
    auto const maxErr       = (f_baud * Num) / Denom;
    return err <= maxErr;
}

// chip_rp_common/pio/ClockOut.hpp:50-67 just before the move to Prescaler.hpp (the divider
// getDiv truncates to), before the migration: the double check with the default tolerance
constexpr bool rpClockOutIsValid(std::uint32_t clockSpeed,
                                 std::uint32_t outputHz) {
    double const clockDiv    = double(clockSpeed) / (2.0 * double(outputHz));
    auto const   div_int     = static_cast<std::uint16_t>(clockDiv);
    auto const   div_frac    = static_cast<std::uint8_t>((clockDiv - div_int) * 256.0);
    double const actualDiv   = double(div_int) + double(div_frac) / 256.0;
    double const achievedHz  = double(clockSpeed) / (2.0 * actualDiv);
    double const errorHz     = achievedHz - double(outputHz);
    double const toleranceHz = double(outputHz) / 1000.0;
    return (errorHz < 0 ? -errorHz : errorHz) <= toleranceHz;
}
}   // namespace Legacy
