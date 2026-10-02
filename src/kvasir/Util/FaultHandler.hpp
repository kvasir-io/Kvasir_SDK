#pragma once
#include "core/Fault.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/Util/Panic.hpp"
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

    namespace detail {
        extern "C" {
        // the image's code and RAM (linker/common*.ld, every Kvasir image has them)
        extern char const _LINKER_INTERN_rom_start_[];
        extern char const _LINKER_INTERN_rom_end_[];
        extern char const _LINKER_INTERN_ram_start_[];
        extern char const _LINKER_INTERN_ram_end_[];
        }

        // Whether the faulting instruction is a BKPT (Thumb T1 0xBExx, Armv6-M ARM DDI0419E A6.7.12,
        // Armv8-M ARM DDI0553B.y C2.4.17) inside the image: read only after the range check, so a
        // garbage stacked PC cannot fault again here.
        inline bool isBkpt(std::uint32_t pc) {
            auto const at = pc & ~1U;
            auto const in = [at](char const* begin, char const* end) {
                return at >= reinterpret_cast<std::uintptr_t>(begin)
                    && at + 2 <= reinterpret_cast<std::uintptr_t>(end);
            };
            if(!in(_LINKER_INTERN_rom_start_, _LINKER_INTERN_rom_end_)
               && !in(_LINKER_INTERN_ram_start_, _LINKER_INTERN_ram_end_))
            {
                return false;
            }
            return (*reinterpret_cast<std::uint16_t const volatile*>(at) & 0xFF00U) == 0xBE00U;
        }
    }   // namespace detail

    inline void RecordAndLog(std::uint32_t const* stack_ptr,
                             std::uint32_t        lr_value) {
        // A panic ends on a bkpt (Panic::halt). Without a debugger, and on the RP2040 also when a debugger
        // resets a core that sits on it, the bkpt escalates to a HardFault: that is the panic's own stop,
        // already named by Panic::lastPanic, not a second failure - no fault record for it, so the next
        // boot does not print "ended in a fault" next to the panic. The clean-up still runs.
        if(Panic::lastPanic.magic == Panic::Record::Magic && detail::isBkpt(stack_ptr[6])) {
            Core::Log(stack_ptr, lr_value);
            return;
        }
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
    // high-water mark and less than the stack.
    //
    // The stack's size is only known after the link (the boot stack takes whatever RAM is
    // left), so the handler caps the reserve at run time: it never goes below the middle of
    // the stack (rounded down to 8 bytes), and the handler always has the lower half of it.
    // Without the cap the 16 KB default would put SP below RAM on a 16 KB part, and the first
    // push would lock the core up without a fault record.
    template<typename TCleanUpAction       = CleanUpActionNone,
             typename TFaultAction         = FaultActionAssert,
             std::size_t StackReserveBytes = 16384,
             typename StackTop             = Startup::PrimaryStackTop>
    struct Handler {
        static constexpr bool primaryStack = std::is_same_v<StackTop, Startup::PrimaryStackTop>;

        // AAPCS: SP 8-byte aligned at every call the handler makes; the stack's ends are
        // (linker/common_stack_body.inc.ld).
        static_assert(StackReserveBytes % 8 == 0,
                      "StackReserveBytes must be a multiple of 8");

#ifdef KVASIR_CORE1_STACK_BYTES
        // util.cmake's CORE1_STACK_SIZE in bytes: the default 16 KB reserve is more than any
        // core 1 stack in use (and 4x the RP2040's scratch-bank one); the run-time cap would
        // halve it, but a core 1 handler is written for its stack and should say what it wants.
        static_assert(primaryStack || StackReserveBytes < KVASIR_CORE1_STACK_BYTES,
                      "Fault::Handler on Core1StackTop: StackReserveBytes must be less than "
                      "CORE1_STACK_SIZE (and more than core 1's stack high-water mark)");
#endif

        // Whether the core has a stack limit register (MSPLIM: Armv8-M mainline). Without
        // one the secondary-stack entry points below have nothing to clear, and the primary
        // ones serve both stacks (the asm takes StackTop's symbols).
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

        // What the entry points below set SP to, before the cap (KVASIR_FAULT_SAFE_SP).
        static constexpr auto GetSafeStackPointer() {
            return static_cast<std::uint32_t>(StackTop::value()) - StackReserveBytes;
        }

// The entry points below take every address from a literal pool of their own (.ltorg) and
// keep what must survive a call in r4-r7: no register operands, so nothing depends on which
// registers the compiler would have picked (a caller-saved one held across the blx would be
// lost). Thumb-1 and Thumb-2 alike (Cortex-M0+ to M33).
//
// KVASIR_FAULT_SAFE_SP: SP = max(top - reserve, (top + bottom) / 2 rounded down to 8), in
// r0-r2. top + bottom overflows only for a stack above 0x7FFFFFFF, outside the Code, SRAM and
// first RAM regions (Armv6-M ARM DDI0419E Table B3-1, Armv8-M ARM DDI0553B.y B8.1); every
// supported part has its RAM in the SRAM region from 0x20000000, so top - reserve does not
// wrap below 0 either.
// gcc hands Thumb-1 inline asm to the assembler in divided syntax unless told otherwise, and
// there `adds`/`lsrs` with three operands do not exist; gcc switches back after the asm.
#define KVASIR_FAULT_ASM_BEGIN ".syntax unified \n"

#define KVASIR_FAULT_SAFE_SP            \
    "ldr r0, =%c[top] - %c[reserve] \n" \
    "ldr r1, =%c[top]               \n" \
    "ldr r2, =%c[bottom]            \n" \
    "adds r1, r1, r2                \n" \
    "lsrs r1, r1, #4                \n" \
    "lsls r1, r1, #3                \n" \
    "cmp r0, r1                     \n" \
    "bhs 3f                         \n" \
    "mov r0, r1                     \n" \
    "3:                             \n" \
    "mov sp, r0                     \n"

#define KVASIR_FAULT_SP_OPERANDS                                                                \
    [top] "i"(StackTop::top), [bottom] "i"(StackTop::bottom), [reserve] "n"(StackReserveBytes), \
      [cleanup] "i"(std::addressof(CleanUpFunc)), [record] "i"(std::addressof(RecordAndLog)),   \
      [fault] "i"(std::addressof(FaultFunc))

        [[gnu::naked]] static void onIsr() {
#if defined(__thumb__) && __ARM_ARCH_ISA_THUMB == 1
            // Thumb1 (Cortex-M0/M0+) compatible version
            asm volatile(KVASIR_FAULT_ASM_BEGIN
                         "mov r4, lr         \n"
                         "movs r1, #4        \n"
                         "mov r0, r4         \n"
                         "tst r0, r1         \n"
                         "beq 1f             \n"
                         "mrs r5, psp        \n"
                         "b 2f               \n"
                         "1:                 \n"
                         "mrs r5, msp        \n"
                         "2:                 \n" KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r4", "r5", "r6");
#else
            // Thumb2 (Cortex-M3/M4/M7+) optimized version
            asm volatile(KVASIR_FAULT_ASM_BEGIN
                         "mov r4, lr         \n"
                         "tst r4, #4         \n"
                         "ite eq             \n"
                         "mrseq r5, msp      \n"
                         "mrsne r5, psp      \n" KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r4", "r5", "r6");
#endif
        }

        [[gnu::naked]] static void onIsrNoLogNoCleanup() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r6");
        }

        [[gnu::naked]] static void onIsrNoLogWithCleanup() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r6");
        }

        // The same three entry points for a non-primary stack, never instantiated on a core
        // without MSPLIM (hasStackLimit). The difference: MSPLIM is
        // cleared before SP moves. The handler's reserve sits below the stack limit the
        // core runs with, so with the limit still armed the first push in here would raise
        // a second stack-overflow fault inside the fault handler, which is a lockup. The
        // primary versions above are left exactly as they are.
        [[gnu::naked]] static void onIsrSecondary() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN
                         "mov r4, lr         \n"
                         "tst r4, #4         \n"
                         "ite eq             \n"
                         "mrseq r5, msp      \n"
                         "mrsne r5, psp      \n"
                         "movs r0, #0        \n"
                         "msr msplim, r0     \n" KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r4", "r5", "r6");
        }

        [[gnu::naked]] static void onIsrNoLogNoCleanupSecondary() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN
                         "movs r0, #0        \n"
                         "msr msplim, r0     \n" KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r6");
        }

        [[gnu::naked]] static void onIsrNoLogWithCleanupSecondary() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN
                         "movs r0, #0        \n"
                         "msr msplim, r0     \n" KVASIR_FAULT_SAFE_SP
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_SP_OPERANDS
                         : "r0", "r1", "r2", "r6");
        }

#undef KVASIR_FAULT_SP_OPERANDS
#undef KVASIR_FAULT_SAFE_SP
#undef KVASIR_FAULT_ASM_BEGIN

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
