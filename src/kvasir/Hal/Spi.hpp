#pragma once

// SPI vocabulary shared by the chip drivers and the queued SPI engine (kvasir_devices
// SPI/QueueCore.hpp).

#include <cstdint>

namespace Kvasir { namespace Hal {
    /// CPOL/CPHA; the PL022's SPO/SPH (TRM DDI0194H 2.4.4), SERCOM CPOL/CPHA (DS40001882L 27.6.2.5).
    enum class ClockMode : std::uint8_t { mode0, mode1, mode2, mode3 };
}}   // namespace Kvasir::Hal
