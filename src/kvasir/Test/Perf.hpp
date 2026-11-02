#pragma once
// The clocks of a perf test (KVASIR_PERF_TEST, Test/Test.hpp), Config::Cycles of a runner:
//
//   ClockCycles<Clock>  microseconds of a Kvasir clock (Clock::now()): coarse, on every core. Unit "us".
//   DwtCycles           the DWT cycle counter (DWT_CYCCNT), where the core has one (KVASIR_CORE_HAS_CYCLE_COUNTER:
//                       Armv7-M, Armv8-M Mainline; Armv6-M's DWT "does not provide any other cycle counter" than
//                       PCSR, DDI0419E D3.7). Unit "cycles". start() reads DWT_CTRL.NOCYCCNT first (bit 25: "If
//                       DWT_CTRL.NOCYCCNT is 1, there is no cycle counter support", DDI0553B.y B14.2.1 rule RFKFP, field in D1.2.58;
//                       the RP2350 data sheet's Table 143 gives it a reset value of 1 although §3.7 lists the
//                       counter - the board answers: 0x40000001 on the RP2350, i.e. present) and then enables the
//                       counter (Core::Debug::enableCycleCounter: DEMCR.TRCENA, DWT_CTRL.CYCCNTENA; CYCCNT is never
//                       written, every sample is a difference). A runner whose DwtCycles cannot start falls back to
//                       ClockCycles and says so in a TEST|NOTE line.
//   CoreCycles<Clock>   DwtCycles where the core has the counter, ClockCycles<Clock> elsewhere: what a test image
//                       that builds for both kinds of core names.
//
// Perf numbers from code that runs from an RP2350's flash carry an XIP term (cache misses) that repeating does not
// average out: compare a perf result only with one from the same image kind (flash vs RAM_ONLY).
#include "kvasir/Port/Capabilities.hpp"

#include <chrono>
#include <cstdint>
#include <string_constant/string_constant.hpp>
#include <type_traits>

#if KVASIR_CORE_HAS_CYCLE_COUNTER
    #include "cortex_common/Debug.hpp"
#endif

namespace Kvasir::Test {
template<typename Clock>
struct ClockCycles {
    static constexpr auto unit = SC_LIFT("us");

    static bool start() { return true; }

    static std::uint32_t now() {
        return static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
    }
};

#if KVASIR_CORE_HAS_CYCLE_COUNTER
struct DwtCycles {
    static constexpr auto unit = SC_LIFT("cycles");

    /// false: this core's DWT has no cycle counter (NOCYCCNT reads 1).
    static bool start() {
        using namespace Kvasir::Register;
        using Dwt = Core::Debug::DwtR;
        if(get<0>(apply(read(Dwt::CTRL::nocyccnt)))
           != Dwt::CTRL::NOCYCCNTVal::cycle_counter_supported)
        {
            return false;
        }
        Core::Debug::enableCycleCounter();
        return true;
    }

    static std::uint32_t now() { return Core::Debug::cycleCount(); }
};

template<typename Clock>
using CoreCycles = DwtCycles;
#else
template<typename Clock>
using CoreCycles = ClockCycles<Clock>;
#endif
}   // namespace Kvasir::Test
