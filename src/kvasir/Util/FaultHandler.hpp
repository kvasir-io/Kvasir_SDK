#pragma once
#include "core/Fault.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
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

    // StackTop: which stack the handler runs on, Startup::PrimaryStackTop (the boot core's,
    // the default and the only choice on a single-core chip) or Startup::Core1StackTop for
    // the handler in a SecondaryCore's list. The handler sets SP to StackTop minus
    // StackReserveBytes and runs down from there, so the top StackReserveBytes of the
    // faulting context survive untouched and are what Core::Fault::Log reads its frame
    // from; a context that had grown deeper than the reserve has its frame under the
    // handler's own pushes. So the reserve wants to be more than the stack's expected
    // high-water mark and less than the stack, which nothing here can check against a
    // linker-sized region: 16 KB is fine for a boot stack that grows to fill RAM, too much
    // for a 16 KB core 1 stack, and 4 KB there is right only while the measured high-water
    // stays below it.
    template<typename TCleanUpAction       = CleanUpActionNone,
             typename TFaultAction         = FaultActionAssert,
             std::size_t StackReserveBytes = 16384,
             typename StackTop             = Startup::PrimaryStackTop>
    struct Handler {
        static constexpr bool primaryStack = std::is_same_v<StackTop, Startup::PrimaryStackTop>;

        // Whether the core has a stack limit register (MSPLIM: Armv8-M mainline). Without
        // one the secondary-stack entry points below have nothing to clear, and the primary
        // ones serve both stacks (GetSafeStackPointer already picks StackTop).
#if defined(__thumb__) && __ARM_ARCH_ISA_THUMB == 1
        static constexpr bool hasStackLimit = false;
#else
        static constexpr bool hasStackLimit = true;
#endif

        // Startup: the handler for a core's stack belongs in that core's list, or its fault
        // stack lands in the other core's region (kvasir/StartUp/Resources.hpp, rule 5).
        static constexpr unsigned startupCore = primaryStack ? 0 : 1;

        static void CleanUpFunc() { TCleanUpAction{}(); }

        static void FaultFunc() { TFaultAction{}(); }

        static constexpr auto GetSafeStackPointer() {
            return static_cast<std::uint32_t>(StackTop::value()) - StackReserveBytes;
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

        // The same three entry points for a non-primary stack, never instantiated on a core
        // without MSPLIM (hasStackLimit). The difference: MSPLIM is
        // cleared before SP moves. The handler's reserve sits below the stack limit the
        // core runs with, so with the limit still armed the first push in here would raise
        // a second stack-overflow fault inside the fault handler, which is a lockup. The
        // primary versions above are left exactly as they are.
        [[gnu::naked]] static void onIsrSecondary() {
            asm volatile(
              "mov r4, lr         \n"
              "tst r4, #4         \n"
              "ite eq             \n"
              "mrseq r5, msp      \n"
              "mrsne r5, psp      \n"
              "movs r0, #0        \n"
              "msr msplim, r0     \n"
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
        }

        [[gnu::naked]] static void onIsrNoLogNoCleanupSecondary() {
            asm volatile(
              "movs r0, #0        \n"
              "msr msplim, r0     \n"
              "mov sp, %0         \n"
              "blx %1             \n"
              :
              : "l"(GetSafeStackPointer()), "l"(std::addressof(FaultFunc))
              : "r0");
        }

        [[gnu::naked]] static void onIsrNoLogWithCleanupSecondary() {
            asm volatile(
              "movs r0, #0        \n"
              "msr msplim, r0     \n"
              "mov sp, %0         \n"
              "blx %1             \n"
              "blx %2             \n"
              :
              : "l"(GetSafeStackPointer()),
                "l"(std::addressof(CleanUpFunc)),
                "l"(std::addressof(FaultFunc))
              : "r0");
        }

        static constexpr auto getIsrFunc() {
#ifdef USE_UC_LOG
            if constexpr(primaryStack || !hasStackLimit) {
                return std::addressof(onIsr);
            } else {
                return std::addressof(onIsrSecondary);
            }
#else
            if constexpr(std::is_same_v<TCleanUpAction, CleanUpActionNone>) {
                if constexpr(primaryStack || !hasStackLimit) {
                    return std::addressof(onIsrNoLogNoCleanup);
                } else {
                    return std::addressof(onIsrNoLogNoCleanupSecondary);
                }
            } else {
                if constexpr(primaryStack || !hasStackLimit) {
                    return std::addressof(onIsrNoLogWithCleanup);
                } else {
                    return std::addressof(onIsrNoLogWithCleanupSecondary);
                }
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
