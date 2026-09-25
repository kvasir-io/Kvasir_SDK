#pragma once
#include "core/Fault.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "uc_log/uc_log.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace Kvasir { namespace Fault {
    /// The registers of the last fault, kept in .noInit across the reset.
    struct Record {
        static constexpr std::uint32_t Magic = 0xFA17'C0DEU;

        std::uint32_t magic;
        std::uint32_t count;   // faults since last reported; registers are the first one's
        std::uint32_t pc;
        std::uint32_t lr;
        std::uint32_t xpsr;
        std::uint32_t excReturn;
        std::uint32_t r0;
        std::uint32_t r1;
        std::uint32_t r2;
        std::uint32_t r3;
        std::uint32_t r12;
    };

    // volatile: uninitialized, LTO would otherwise shrink `magic` to one bit
    [[gnu::section(".noInit"), gnu::used]] inline Record volatile lastFault;

    inline std::optional<Record> takeLastFault() {
        if(lastFault.magic != Record::Magic) { return std::nullopt; }
        Record const r{.magic     = Record::Magic,
                       .count     = lastFault.count,
                       .pc        = lastFault.pc,
                       .lr        = lastFault.lr,
                       .xpsr      = lastFault.xpsr,
                       .excReturn = lastFault.excReturn,
                       .r0        = lastFault.r0,
                       .r1        = lastFault.r1,
                       .r2        = lastFault.r2,
                       .r3        = lastFault.r3,
                       .r12       = lastFault.r12};
        lastFault.magic = 0;
        return r;
    }

    inline void logLastFault() {
        [[maybe_unused]] auto const fault = takeLastFault();
        if(fault) {
            UC_LOG_E(
              "the run before this one ended in a fault ({} since the last report, this is the "
              "first): PC={:#010x} LR={:#010x} xPSR={:#010x} EXC_RETURN={:#010x} R0={:#010x} "
              "R1={:#010x} R2={:#010x} R3={:#010x} R12={:#010x}",
              fault->count,
              fault->pc,
              fault->lr,
              fault->xpsr,
              fault->excReturn,
              fault->r0,
              fault->r1,
              fault->r2,
              fault->r3,
              fault->r12);
        }
    }

    inline void RecordAndLog(std::uint32_t const* stack_ptr,
                             std::uint32_t        lr_value) {
        // keep the first fault: later ones before the next boot are its consequences
        if(lastFault.magic == Record::Magic) {
            lastFault.count = lastFault.count + 1;
        } else {
            lastFault.count     = 1;
            lastFault.r0        = stack_ptr[0];
            lastFault.r1        = stack_ptr[1];
            lastFault.r2        = stack_ptr[2];
            lastFault.r3        = stack_ptr[3];
            lastFault.r12       = stack_ptr[4];
            lastFault.lr        = stack_ptr[5];
            lastFault.pc        = stack_ptr[6];
            lastFault.xpsr      = stack_ptr[7];
            lastFault.excReturn = lr_value;
            lastFault.magic     = Record::Magic;
        }
        Core::Log(stack_ptr, lr_value);
    }

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
    // faulting context survive untouched and are what Core::Log (the chip's Fault::Core) reads its frame
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
                "i"(std::addressof(RecordAndLog)),
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
                "i"(std::addressof(RecordAndLog)),
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
                "i"(std::addressof(RecordAndLog)),
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

        static constexpr auto earlyInit = Core::EarlyInitList{};

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
