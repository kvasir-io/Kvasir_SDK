#pragma once
// The name of an interrupt index, for compile errors (Resources.hpp's reports): a core exception by its
// architectural name, a device interrupt by its SVD name (peripherals/InterruptNames.hpp from svd_converter; without
// it, device interrupts have no name). Constant-expression only.
#include <array>
#include <cstdint>
#include <string_view>

#if __has_include(<peripherals/InterruptNames.hpp>)
    #include <peripherals/InterruptNames.hpp>
    #define KVASIR_DETAIL_HAS_INTERRUPT_NAMES 1
#else
    #define KVASIR_DETAIL_HAS_INTERRUPT_NAMES 0
#endif

namespace Kvasir::Startup::Diagnostics {
/// Kvasir's index is the exception number minus 16 (Nvic::Index; core_cortex_common CoreInterrupts.hpp). The core
/// exceptions: Armv8-M ARM DDI0553B.y B3.9 "Exception numbers and exception priority numbers" (md l.3929-3960);
/// Armv6-M has NMI, HardFault, SVCall, PendSV, SysTick of these (DDI0419E B1.5.2 Table B1-3, md l.7029).
consteval std::string_view interruptName(int index) {
    switch(index) {
    case -14: return "NMI";
    case -13: return "HardFault";
    case -12: return "MemManage";
    case -11: return "BusFault";
    case -10: return "UsageFault";
    case -9:  return "SecureFault";
    case -5:  return "SVCall";
    case -4:  return "DebugMonitor";
    case -2:  return "PendSV";
    case -1:  return "SysTick";
    default:  break;
    }
#if KVASIR_DETAIL_HAS_INTERRUPT_NAMES
    for(auto const& irq : ::Kvasir::Peripheral::interruptNames) {
        if(static_cast<int>(irq.index) == index) { return irq.name; }
    }
#endif
    return {};
}
}   // namespace Kvasir::Startup::Diagnostics
