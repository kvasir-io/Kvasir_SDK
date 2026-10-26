#pragma once
// The name of an interrupt index, for compile errors (Resources.hpp's reports): a core exception by its
// architectural name, a device interrupt by its SVD name. Includes nothing generated: the device names are a hook,
// DeviceInterruptNames<Tag::User>, that svd_converter's generated peripherals/InterruptNames.hpp specialises (every
// chip has it through chip/Interrupt.hpp -> peripherals/Interrupts.hpp); looked up only when a report is built.
// Constant-expression only.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"

#include <string_view>

namespace Kvasir::Startup::Diagnostics {
/// Kvasir's index is the exception number minus 16 (Nvic::Index; core_cortex_common CoreInterrupts.hpp). The core
/// exceptions: Armv8-M ARM DDI0553B.y B3.9 "Exception numbers and exception priority numbers" (md l.3929-3960);
/// Armv6-M has NMI, HardFault, SVCall, PendSV, SysTick of these (DDI0419E B1.5.2 Table B1-3, md l.7029).
consteval std::string_view coreExceptionName(int index) {
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
    return {};
}

/// The chip's device interrupt names (the hook form, Port/Hooks.hpp): specialised for Tag::User by the generated
/// peripherals/InterruptNames.hpp; without it (host tests) a device interrupt has no name.
template<typename T, typename... Ts>
struct DeviceInterruptNames {
    static consteval std::string_view of(int) { return {}; }
};

/// The name of `index` for a report about Rule: the hook is instantiated only here, when a report is built.
template<typename Rule>
consteval std::string_view interruptName(int index) {
    if(auto const core = coreExceptionName(index); !core.empty()) { return core; }
    return Port::LateT<DeviceInterruptNames<Tag::User>, Rule>::of(index);
}
}   // namespace Kvasir::Startup::Diagnostics
