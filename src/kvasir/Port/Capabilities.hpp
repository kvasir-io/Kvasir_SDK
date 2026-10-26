#pragma once
// What the core under this image provides, as `KVASIR_CORE_*` macros (each 0 or 1) for `#if` tests above the core
// layer - never `if constexpr`: a discarded branch in a non-template function still names its symbols. Derived once
// from the compiler's architecture macros; a build may override one with a definition. The host: all 0. Each core
// package's core/Features.hpp asserts that its core agrees.
//   ARMV6M            Armv6-M (Cortex-M0+)
//   ARMV7EM           Armv7E-M (Cortex-M4)
//   ARMV8M_MAIN       Armv8-M Mainline (Cortex-M33)
//   HAS_CYCLE_COUNTER DWT_CYCCNT (Armv7-M, Armv8-M Mainline; the Armv6-M DWT has none, DDI0419E Table C1-18)
//   HAS_DEBUGEN_READ  DHCSR.C_DEBUGEN readable by software (DDI0553B.y D1.2.39; the Cortex-M33 TRM lists it for
//                     software; Armv7-M leaves it IMPLEMENTATION DEFINED, DDI0403E C1.6.2: 0 on the M4)
//   HAS_STACK_LIMITS  MSPLIM/PSPLIM (Armv8-M, DDI0553B.y D1.2.177)
//   HAS_FAULT_STATUS  CFSR/HFSR/MMFAR/BFAR (Armv7-M, Armv8-M Mainline)

#ifndef KVASIR_CORE_ARMV6M
    #ifdef __ARM_ARCH_6M__
        #define KVASIR_CORE_ARMV6M 1
    #else
        #define KVASIR_CORE_ARMV6M 0
    #endif
#endif
#ifndef KVASIR_CORE_ARMV7EM
    #ifdef __ARM_ARCH_7EM__
        #define KVASIR_CORE_ARMV7EM 1
    #else
        #define KVASIR_CORE_ARMV7EM 0
    #endif
#endif
#ifndef KVASIR_CORE_ARMV8M_MAIN
    #ifdef __ARM_ARCH_8M_MAIN__
        #define KVASIR_CORE_ARMV8M_MAIN 1
    #else
        #define KVASIR_CORE_ARMV8M_MAIN 0
    #endif
#endif
#ifndef KVASIR_CORE_HAS_CYCLE_COUNTER
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        #define KVASIR_CORE_HAS_CYCLE_COUNTER 1
    #else
        #define KVASIR_CORE_HAS_CYCLE_COUNTER 0
    #endif
#endif
#ifndef KVASIR_CORE_HAS_DEBUGEN_READ
    #ifdef __ARM_ARCH_8M_MAIN__
        #define KVASIR_CORE_HAS_DEBUGEN_READ 1
    #else
        #define KVASIR_CORE_HAS_DEBUGEN_READ 0
    #endif
#endif
#ifndef KVASIR_CORE_HAS_STACK_LIMITS
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_8M_BASE__)
        #define KVASIR_CORE_HAS_STACK_LIMITS 1
    #else
        #define KVASIR_CORE_HAS_STACK_LIMITS 0
    #endif
#endif
#ifndef KVASIR_CORE_HAS_FAULT_STATUS
    #if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__)
        #define KVASIR_CORE_HAS_FAULT_STATUS 1
    #else
        #define KVASIR_CORE_HAS_FAULT_STATUS 0
    #endif
#endif
