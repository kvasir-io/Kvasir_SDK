#pragma once
// What kind of reset started this run, as the boot guard (Runtime/BootGuard.hpp) judges it. Each chip package maps its
// own reset-cause registers onto it in `Kvasir::PM::resetKind()`. It is never stored: only the run that reads it
// judges by it.
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
    unknown,
    lowPower   // a low-power entry turned into a reset (STM32F4: Standby or Stop entered with nRST_STDBY / nRST_STOP
               // cleared in the option bytes, RM0430 6.1.1)
};
}   // namespace Kvasir

namespace Kvasir::Port {
// What a fallback declaration returns: a contract piece the core or chip package does not provide (Port/Check.hpp).
struct Missing {};
}   // namespace Kvasir::Port

namespace Kvasir::PM {
// Chosen only when the chip package declares no `ResetKind resetKind()` of its own (a non-template function always
// wins over a template, [over.match.best]); declared, never defined: BootGuard names the missing piece instead of
// "no member named 'resetKind'". The chip's declaration must come before the caller's definition.
template<typename = void>
Kvasir::Port::Missing resetKind();
}   // namespace Kvasir::PM
