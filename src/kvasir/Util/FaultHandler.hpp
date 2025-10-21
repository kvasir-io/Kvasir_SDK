#pragma once
#include "core/Fault.hpp"
#include "uc_log/uc_log.hpp"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace Kvasir { namespace Fault {
    struct CleanUpActionNone {
        void operator()() {}
    };

    struct FaultActionAssert {
        [[noreturn]] void operator()() {
            while(true) { asm("bkpt 6" : : :); }
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
              "mov r4, lr         \n"
              "movs r1, #4        \n"
              "mov r0, r4         \n"
              "tst r0, r1         \n"
              "beq 1f             \n"
              "mrs r5, psp        \n"
              "b 2f               \n"
              "1:                 \n"
              "mrs r5, msp        \n"
              "2:                 \n"
              "mov sp, %0         \n"
              "blx %1             \n"
              "mov r0, r5         \n"
              "mov r1, r4         \n"
              "bl %2              \n"
              "blx %3             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "i"(std::addressof(Core::Fault::Log)),
                "l"(std::addressof(FaultFunc))
              : "r0", "r1", "r2", "r4", "r5");
#else
            // Thumb2 (Cortex-M3/M4/M7+) optimized version
            asm volatile(
              "mov r4, lr         \n"
              "tst r4, #4         \n"
              "ite eq             \n"
              "mrseq r5, msp      \n"
              "mrsne r5, psp      \n"
              "mov sp, %0         \n"
              "blx %1             \n"
              "mov r0, r5         \n"
              "mov r1, r4         \n"
              "bl %2              \n"
              "blx %3             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "i"(std::addressof(Core::Fault::Log)),
                "l"(std::addressof(FaultFunc))
              : "r0", "r1", "r2", "r4", "r5");
#endif
        }

        [[gnu::naked]] static void onIsrNoLogNoCleanup() {
            asm volatile(
              "mov sp, %0         \n"
              "blx %1             \n"
              :
              : "l"(GetSafeStackPointer()), "l"(std::addressof(FaultFunc))
              :);
        }

        [[gnu::naked]] static void onIsrNoLogWithCleanup() {
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

        static constexpr auto getIsrFunc() {
#ifdef USE_UC_LOG
            return std::addressof(onIsr);
#else
            if constexpr(std::is_same_v<TCleanUpAction, CleanUpActionNone>) {
                return std::addressof(onIsrNoLogNoCleanup);
            } else {
                return std::addressof(onIsrNoLogWithCleanup);
            }
#endif
        }

        static constexpr auto earlyInit = Core::Fault::EarlyInitList{};

        static constexpr auto initStepPeripheryEnable = MPL::list(
          Nvic::makeEnable(Nvic::InterruptOffsetTraits<>::FaultInterruptIndexsNeedEnable{}));

        template<typename... Ts>
        static constexpr auto makeIsr(brigand::list<Ts...>) {
            return brigand::list<Kvasir::Nvic::Isr<getIsrFunc(), Nvic::Index<Ts::value>>...>{};
        }

        using Isr = decltype(makeIsr(
          typename Kvasir::Nvic::InterruptOffsetTraits<>::FaultInterruptIndexs{}));
    };
}}   // namespace Kvasir::Fault

