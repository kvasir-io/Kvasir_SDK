#pragma once
#include "core/Fault.hpp"
#include "uc_log/uc_log.hpp"

#include <cassert>
#include <cstdint>

namespace Kvasir { namespace Fault {
    struct CleanUpActionNone {
        void operator()() {}
    };

    struct FaultActionAssert {
        [[noreturn]] void operator()() {
            while(true) {
                asm("bkpt 6" : : :);
            }
        }
    };

    template<typename TCleanUpAction       = CleanUpActionNone,
             typename TFaultAction         = FaultActionAssert,
             std::size_t StackReserveBytes = 16384>
    struct Handler {
        static void CleanUpFunc() { TCleanUpAction{}(); }

        static void FaultFunc() { TFaultAction{}(); }

        static constexpr auto GetSafeStackPointer() {
            return reinterpret_cast<std::uint32_t>(std::addressof(_LINKER_stack_end_))
                 - StackReserveBytes;
        }

        [[gnu::naked]] static void onIsr() {
#if defined(__thumb__) && __ARM_ARCH_ISA_THUMB == 1
            // Thumb1 (Cortex-M0/M0+) compatible version
            asm volatile(
              "mov sp, %0         \n"
              "blx %1             \n"
              "mov r0, lr         \n"
              "movs r1, #4        \n"
              "tst r0, r1         \n"
              "beq 1f             \n"
              "mrs r0, psp        \n"
              "b 2f               \n"
              "1:                 \n"
              "mrs r0, msp        \n"
              "2:                 \n"
              "mov r1, lr         \n"
              "bl %2              \n"
              "blx %3             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "i"(std::addressof(Core::Fault::Log)),
                "l"(std::addressof(FaultFunc))
              : "r0", "r1", "r2");
#else
            // Thumb2 (Cortex-M3/M4/M7+) optimized version
            asm volatile(
              "mov sp, %0         \n"
              "blx %1             \n"
              "tst lr, #4         \n"
              "ite eq             \n"
              "mrseq r0, msp      \n"
              "mrsne r0, psp      \n"
              "mov r1, lr         \n"
              "bl %2              \n"
              "blx %3             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "i"(std::addressof(Core::Fault::Log)),
                "l"(std::addressof(FaultFunc))
              : "r0", "r1", "r2");
#endif
        }

        [[gnu::naked]] static void onIsrNoLog() {
            asm volatile(
              "mov sp, %0         \n"
              "blx %1             \n"
              "blx %2             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "l"(std::addressof(FaultFunc))
              :);
        }

        //     static constexpr auto earlyInit = Core::Fault::EarlyInitList{};

        static constexpr auto initStepPeripheryEnable = MPL::list(
          Nvic::makeEnable(Nvic::InterruptOffsetTraits<>::FaultInterruptIndexsNeedEnable{}));

        template<typename... Ts>
        static constexpr auto makeIsr(brigand::list<Ts...>) {
#ifdef USE_UC_LOG
            return brigand::list<
              Kvasir::Nvic::Isr<std::addressof(onIsr), Nvic::Index<Ts::value>>...>{};
#else
            return brigand::list<
              Kvasir::Nvic::Isr<std::addressof(onIsrNoLog), Nvic::Index<Ts::value>>...>{};
#endif
        }

        using Isr = decltype(makeIsr(
          typename Kvasir::Nvic::InterruptOffsetTraits<>::FaultInterruptIndexs{}));
    };
}}   // namespace Kvasir::Fault
