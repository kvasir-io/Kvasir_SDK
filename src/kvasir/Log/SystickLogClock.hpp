#pragma once
// The common log clock of an image, for its config header (kvasir_config.hpp, UC_LOG_CONFIG_HEADER): the image names
// its clock and keeps its uc_log::LogClock<uc_log::Tag::User> specialisation, one line over this
//
//   using SystickClock = Kvasir::Log::SystickLogClock<ClockSpeed>;
//   template<> struct uc_log::LogClock<uc_log::Tag::User> : SystickClock {};
//
// and a clock of another shape (another clock base, a second core's SysTick, a TIMER) is spelled out. An alias of the
// core's SystickClockBase. Its backend: kvasir/Log/RttSink.hpp.
//
// Include it after what the chip's core/Systick.hpp needs first (the RP chip variant: chip/rp_common/PinConfig.hpp).

#include <chrono>
#include <core/Systick.hpp>

namespace Kvasir::Log {

namespace Detail {
    template<auto ClockSpeed, int MinOverrunYears>
    struct SystickLogClockConfig {
        static constexpr auto clockBase      = Kvasir::Systick::useProcessorClock;
        static constexpr auto clockSpeed     = ClockSpeed;
        static constexpr auto minOverrunTime = std::chrono::years(MinOverrunYears);
    };
}   // namespace Detail

/// The core's SysTick counting the processor clock (ClockSpeed Hz) into a std::chrono time point, its overrun counter
/// wide enough for MinOverrunYears years (core_cortex_common Systick.hpp: 32 bits while that fits, else 64). Per core:
/// read it on the core whose SysTick interrupt advances it.
template<auto ClockSpeed, int MinOverrunYears = 15>
using SystickLogClock
  = Kvasir::Systick::SystickClockBase<Detail::SystickLogClockConfig<ClockSpeed, MinOverrunYears>>;

}   // namespace Kvasir::Log
