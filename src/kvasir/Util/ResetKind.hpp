#pragma once
// What kind of reset started this run, as the boot guard (Util/BootGuard.hpp) judges it. Each chip package maps its
// own reset-cause registers onto it in `Kvasir::PM::resetKind()`.
#include <cstdint>

namespace Kvasir {
enum class ResetKind : std::uint8_t {
    powerOn,           // power-on, a power domain coming back
    brownOut,          // brown-out, a supply glitch
    external,          // the RUN / reset pin
    watchdogTimeout,   // the watchdog's timer ran out (on the RP2350 also the bootrom's reboot)
    watchdogForced,    // a watchdog reset software forced
    softwareRequest,   // SYSRESETREQ from the application
    debugger,          // a reset the debug port asked for
    unknown
};
}   // namespace Kvasir
