#pragma once
// What a core package (core_cortex_m0plus, core_cortex_m33, core_cortex_m4) provides, as `core/X.hpp` on the
// include path; the shared part is core_cortex_common, which each core forwards - except Debug.hpp, one file for every
// core that is included as `cortex_common/Debug.hpp`:
//   core/core.hpp                            the NVIC/SCB/SysTick register headers and the module headers below
//   core/Nvic.hpp, SystemControl.hpp,        NVIC actions, LineMask/CountedLineMask, the per-line InterruptGuard;
//   Systick.hpp, Barrier.hpp                 system reset; SystickClockBase; barriers
//   core/InterruptMask.hpp, Special.hpp      PRIMASK (Core::IrqGuard, IrqState: msr + isb restore); IPSR, HaltingDebug
//   core/CoreInterrupts.hpp                  Kvasir::CoreInterrupts (the exception objects), Core::InterruptRules
//   core/Features.hpp                        asserts its core agrees with Port/Capabilities.hpp's KVASIR_CORE_*
//   cortex_common/Debug.hpp                  Core::haltingDebug() (off/on/unknown); the cycle counter where
//                                            KVASIR_CORE_HAS_CYCLE_COUNTER
//   core/StartUp.hpp                         Startup::Core::{startup, cleanCore, SecondaryCoreTraits}; cleanCore
//                                            and SecondaryCoreTraits from cortex_common (one body for every core);
//                                            startup(): FPU access (m4, m33), MSPLIM (m33)
//   core/Fault.hpp                           Fault::Core::Snapshot snapshot(), void clearFaultStatus(),
//                                            void log(std::uint32_t const* frame, std::uint32_t excReturn,
//                                            Snapshot const&) (the COREFAULT line), EarlyInitList (Startup actions)
//   cmake/core.cmake                         the target flags and the core SVD's svd_convert()
// A core header names no chip header: a chip fact (Nvic::InterruptOffsetTraits<Tag::User>) is read when a firmware
// instantiates something.
//
// The core functions the SDK calls from Startup have a fallback here, chosen only when the core lacks the function
// (cleanCore: a template, which the core's non-template beats, [over.match.best]); Port/Check.hpp then names the
// missing piece instead of "no member named ...". StartUp.hpp includes core/StartUp.hpp and core/Nvic.hpp itself, so
// the core's declaration is always seen before a call binds (a non-dependent call binds where it is written).
#include "kvasir/Port/ResetKind.hpp"   // Port::Missing

namespace Kvasir::Startup::Core {
template<typename = void>
Kvasir::Port::Missing cleanCore();
}   // namespace Kvasir::Startup::Core

// The core's Nvic::clearAllEnablesAndPending() is a template itself (it reads the chip's traits where it is called):
// this fallback sits in a namespace of its own that Nvic nominates, which qualified lookup searches only when Nvic
// has no member of that name ([namespace.qual]).
namespace Kvasir::Port::Fallback {
template<typename...>
Kvasir::Port::Missing clearAllEnablesAndPending();
}   // namespace Kvasir::Port::Fallback

namespace Kvasir::Nvic { using namespace Kvasir::Port::Fallback; }   // namespace Kvasir::Nvic
