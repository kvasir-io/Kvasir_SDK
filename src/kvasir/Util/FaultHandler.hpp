#pragma once
#include "core/Fault.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/Util/CrashRecord.hpp"
#include "kvasir/Util/Panic.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
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

    /// The fault record under CrashRecord::Full: the whole state at the fault, CRC-checked
    /// (Persistent.hpp). Referenced only by the Full path, so a Legacy image does not have it.
    struct RecordTag {
        static constexpr std::uint32_t id      = persistentId("KFLT");
        static constexpr std::uint16_t version = 2;
    };

    struct FullRecord {
        enum Flag : std::uint32_t {
            frameValid = 1U << 0,   // r0-r3, r12, lr, pc, xpsr read from a stacked frame inside RAM
            calleeSaved = 1U << 1,   // r4-r11, msp, psp, control captured at the handler's entry
            faultRegs   = 1U << 2,   // cfsr, hfsr, mmfar, bfar, shcsr (Armv7-M, Armv8-M main)
            fpFrame     = 1U << 3,   // EXC_RETURN.FType == 0: the extended (FP) frame was stacked
            stackLimits = 1U << 4,   // msplim, psplim (Armv8-M main)
            secureRegs  = 1U << 5,   // sfsr, sfar (Armv8-M with the Security Extension)
            stackCopied = 1U << 6,   // Fault::lastStack holds a snapshot
        };

        static constexpr unsigned coreShift
          = 8;   // bits 11:8 the core (0, 1 = the secondary stack)
        static constexpr unsigned archShift = 12;   // bits 15:12 the architecture (6, 7, 8)

        std::uint32_t count;   // faults since the last report; the registers are the first one's
        std::uint32_t flags;
        std::uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr;   // the stacked frame
        std::uint32_t r4, r5, r6, r7, r8, r9, r10, r11;    // at the handler's entry
        std::uint32_t excReturn, msp, psp, control;
        std::uint32_t sp;   // the faulting context's SP before the frame was pushed; 0: unknown
        std::uint32_t cfsr, hfsr, mmfar, bfar, shcsr, icsr;
        std::uint32_t sfsr, sfar, msplim, psplim;
    };

    static_assert(sizeof(FullRecord) == 33 * 4,
                  "the layout the host tools decode");

    [[KVASIR_SECTION(".noInit")]] inline Persistent<FullRecord, RecordTag> lastFaultV2;

    /// A copy of the faulting stack (CrashRecord::Options::stackBytes): `bytes` valid bytes from
    /// `from`, the rest 0. lastStack<N>: N is the policy's stackBytes.
    template<std::uint16_t Bytes>
    struct StackSnapshot {
        std::uint32_t from;
        std::uint32_t bytes;
        std::uint32_t words[Bytes / 4];
    };

    struct StackTag {
        static constexpr std::uint32_t id      = persistentId("KSTK");
        static constexpr std::uint16_t version = 1;
    };

    template<std::uint16_t Bytes>
    [[KVASIR_SECTION(".noInit")]] inline Persistent<StackSnapshot<Bytes>, StackTag> lastStack;

    namespace Detail {
        template<typename... Dummy>
        inline constexpr bool fullPolicy = CrashRecord::Detail::isFull<
          std::remove_cvref_t<decltype(CrashRecord::injectedPolicy<Dummy...>)>>;

        /// Whether a fault record is there, without taking it.
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        bool faultRecorded() {
            if constexpr(fullPolicy<Dummy...>) {
                return lastFaultV2.valid();
            } else {
                return lastFault.magic == Record::Magic;
            }
        }
    }   // namespace Detail

    /// The record, once: cleared by reading. The same shape under either policy (Full: the
    /// stacked frame and EXC_RETURN of lastFaultV2, which takeLastFullFault() gives whole).
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline std::optional<Record> takeLastFault() {
        if constexpr(Detail::fullPolicy<Dummy...>) {
            auto const f = lastFaultV2.take();
            if(!f) { return std::nullopt; }
            return Record{.magic     = Record::Magic,
                          .count     = f->count,
                          .pc        = f->pc,
                          .lr        = f->lr,
                          .xpsr      = f->xpsr,
                          .excReturn = f->excReturn,
                          .r0        = f->r0,
                          .r1        = f->r1,
                          .r2        = f->r2,
                          .r3        = f->r3,
                          .r12       = f->r12};
        } else {
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
    }

    /// The full record, once (CrashRecord::Full only).
    inline std::optional<FullRecord> takeLastFullFault() { return lastFaultV2.take(); }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline void logLastFault() {
        [[maybe_unused]] auto const fault = takeLastFault<Dummy...>();
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

        // Whether `bytes` from `at` lie inside the image's RAM: the only memory the Full record
        // reads through a pointer it got from the faulting context.
        inline bool inRam(std::uint32_t at,
                          std::uint32_t bytes) {
            auto const begin = reinterpret_cast<std::uintptr_t>(_LINKER_INTERN_ram_start_);
            auto const end   = reinterpret_cast<std::uintptr_t>(_LINKER_INTERN_ram_end_);
            return (at & 3U) == 0 && at >= begin && at <= end && bytes <= end - at;
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

    namespace Detail {
        // What the Full entry points save before anything else runs, per handler (so per core):
        // r4-r11, EXC_RETURN, MSP, PSP, CONTROL, MSPLIM, PSPLIM. A fixed .bss address costs no
        // stack, which matters when the fault is a stack overflow.
        template<typename StackTop>
        inline std::uint32_t entryRegs[14];

        inline constexpr std::size_t entryExcReturn = 8, entryMsp = 9, entryPsp = 10,
                                     entryControl = 11, entryMsplim = 12, entryPsplim = 13;
    }   // namespace Detail

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

        // The whole fault state into Fault::lastFaultV2 (and the stack into lastStack), called by
        // the Full entry points on the fault stack BEFORE the clean-up action (record first:
        // the clean-up must not change what is recorded). Reads nothing through a pointer from
        // the faulting context that is not inside RAM, so it cannot fault again here.
        static void RecordFull(std::uint32_t const* frame,
                               std::uint32_t        excReturn) {
            using Policy       = CrashRecord::Detail::PolicyFor<StackTop>;
            constexpr auto O   = Policy::options;
            auto const     sys = Core::snapshot();
            auto const&    e   = Detail::entryRegs<StackTop>;
            auto const     at  = reinterpret_cast<std::uintptr_t>(frame);
            // MSTKERR, STKERR, STKOF (CFSR bits 4, 12, 20; DDI0553B.y D1.2.11): the frame was not
            // (wholly) pushed. Armv6-M has no CFSR (0): the RAM check is all there is.
            constexpr std::uint32_t stackingErrors = (1U << 4) | (1U << 12) | (1U << 20);
            bool const frameOk = (sys.cfsr & stackingErrors) == 0 && detail::inRam(at, 8 * 4);

            // a panic's own bkpt escalating (RecordAndLog's rule): the panic record names it
            if(Panic::Detail::panicRecorded<>() && frameOk && detail::isBkpt(frame[6])) {
                logFull(frame, excReturn, frameOk);
                return;
            }
            // keep the first fault: later ones before the next boot are its consequences
            if(auto old = lastFaultV2.load()) {
                old->count = old->count + 1;
                lastFaultV2.store(*old);
                logFull(frame, excReturn, frameOk);
                return;
            }

            FullRecord r{};
            r.count = 1;
            r.flags = (static_cast<std::uint32_t>(__ARM_ARCH) << FullRecord::archShift)
                    | (std::uint32_t{startupCore} << FullRecord::coreShift)
                    | FullRecord::calleeSaved
                    | (sys.faultRegs ? std::uint32_t{FullRecord::faultRegs} : 0U)
                    | (sys.secureRegs ? std::uint32_t{FullRecord::secureRegs} : 0U)
                    | ((excReturn & (1U << 4)) == 0 ? std::uint32_t{FullRecord::fpFrame} : 0U);
            if(frameOk) {
                r.r0   = frame[0];
                r.r1   = frame[1];
                r.r2   = frame[2];
                r.r3   = frame[3];
                r.r12  = frame[4];
                r.lr   = frame[5];
                r.pc   = frame[6];
                r.xpsr = frame[7];
                // the frame: 8 words, 26 with the FP state (EXC_RETURN.FType 0), one more when the
                // processor aligned the stack (stacked xPSR bit 9; DDI0403E.e B1.5.7)
                std::uint32_t const bytes = ((excReturn & (1U << 4)) == 0 ? 26U : 8U) * 4U
                                          + ((r.xpsr & (1U << 9)) != 0 ? 4U : 0U);
                r.sp                      = static_cast<std::uint32_t>(at) + bytes;
                r.flags |= FullRecord::frameValid;
            }
            r.r4        = e[0];
            r.r5        = e[1];
            r.r6        = e[2];
            r.r7        = e[3];
            r.r8        = e[4];
            r.r9        = e[5];
            r.r10       = e[6];
            r.r11       = e[7];
            r.excReturn = excReturn;
            r.msp       = e[Detail::entryMsp];
            r.psp       = e[Detail::entryPsp];
            r.control   = e[Detail::entryControl];
#if defined(__ARM_ARCH_8M_MAIN__)
            r.msplim = e[Detail::entryMsplim];
            r.psplim = e[Detail::entryPsplim];
            r.flags |= FullRecord::stackLimits;
#endif
            r.cfsr  = sys.cfsr;
            r.hfsr  = sys.hfsr;
            r.mmfar = sys.mmfar;
            r.bfar  = sys.bfar;
            r.shcsr = sys.shcsr;
            r.icsr  = sys.icsr;
            r.sfsr  = sys.sfsr;
            r.sfar  = sys.sfar;

            if constexpr(O.stackBytes != 0) {
                // the SP the frame went to; without a valid frame (an overflow) the one EXC_RETURN.SPSEL
                // names, as the core left it
                std::uint32_t const from
                  = (r.sp != 0 ? r.sp : ((excReturn & (1U << 2)) != 0 ? r.psp : r.msp)) & ~3U;
                auto const ramEnd
                  = reinterpret_cast<std::uintptr_t>(detail::_LINKER_INTERN_ram_end_);
                std::uint32_t const n
                  = detail::inRam(from, 0)
                    ? std::min<std::uint32_t>(O.stackBytes,
                                              static_cast<std::uint32_t>(ramEnd - from))
                    : 0U;
                lastStack<O.stackBytes>.storeWords([from, n](std::size_t i) -> std::uint32_t {
                    if(i == 0) { return from; }
                    if(i == 1) { return n; }
                    auto const offset = static_cast<std::uint32_t>((i - 2) * 4);
                    return offset < n
                           ? *reinterpret_cast<std::uint32_t const volatile*>(from + offset)
                           : 0U;
                });
                r.flags |= FullRecord::stackCopied;
            }
            lastFaultV2.store(r);
            logFull(frame, excReturn, frameOk);
        }

        static void logFull([[maybe_unused]] std::uint32_t const* frame,
                            [[maybe_unused]] std::uint32_t        excReturn,
                            [[maybe_unused]] bool                 frameOk) {
#ifdef USE_UC_LOG
            if(frameOk) { Core::Log(frame, excReturn); }
#endif
        }

// The Full entry points save r4-r11, EXC_RETURN, MSP, PSP, CONTROL (and on Armv8-M main MSPLIM,
// PSPLIM) into Detail::entryRegs<StackTop> first, before the sequence below touches r4-r7; r0-r3
// and r12 are in the stacked frame already. Same layout on both: words 0-7 r4-r11, 8 EXC_RETURN,
// 9 MSP, 10 PSP, 11 CONTROL, 12 MSPLIM, 13 PSPLIM. Then record (RecordFull), then the clean-up,
// then the fault action.
#if defined(__thumb__) && __ARM_ARCH_ISA_THUMB == 1
    #define KVASIR_FAULT_SAVE_ENTRY           \
        "ldr r0, =%c[entry]               \n" \
        "stmia r0!, {r4-r7}               \n" \
        "mov r1, r8                       \n" \
        "mov r2, r9                       \n" \
        "mov r3, r10                      \n" \
        "stmia r0!, {r1-r3}               \n" \
        "mov r1, r11                      \n" \
        "mov r2, lr                       \n" \
        "mrs r3, msp                      \n" \
        "stmia r0!, {r1-r3}               \n" \
        "mrs r1, psp                      \n" \
        "mrs r2, control                  \n" \
        "stmia r0!, {r1, r2}              \n"
    #define KVASIR_FAULT_PICK_FRAME \
        "mov r4, lr         \n"     \
        "movs r1, #4        \n"     \
        "mov r0, r4         \n"     \
        "tst r0, r1         \n"     \
        "beq 1f             \n"     \
        "mrs r5, psp        \n"     \
        "b 2f               \n"     \
        "1:                 \n"     \
        "mrs r5, msp        \n"     \
        "2:                 \n"
#else
    #if defined(__ARM_ARCH_8M_MAIN__)
        #define KVASIR_FAULT_SAVE_LIMITS \
            "mrs r1, msplim     \n"      \
            "mrs r2, psplim     \n"      \
            "strd r1, r2, [r0, #12] \n"
    #else
        #define KVASIR_FAULT_SAVE_LIMITS ""
    #endif
    #define KVASIR_FAULT_SAVE_ENTRY                          \
        "ldr r0, =%c[entry]     \n"                          \
        "stm r0, {r4-r11}       \n"                          \
        "str lr, [r0, #32]      \n"                          \
        "mrs r1, msp            \n"                          \
        "mrs r2, psp            \n"                          \
        "mrs r3, control        \n"                          \
        "add r0, r0, #36        \n"                          \
        "stm r0, {r1-r3}        \n" KVASIR_FAULT_SAVE_LIMITS
    #define KVASIR_FAULT_PICK_FRAME \
        "mov r4, lr         \n"     \
        "tst r4, #4         \n"     \
        "ite eq             \n"     \
        "mrseq r5, msp      \n"     \
        "mrsne r5, psp      \n"
#endif

#define KVASIR_FAULT_FULL_OPERANDS                                                              \
    [top] "i"(StackTop::top), [bottom] "i"(StackTop::bottom), [reserve] "n"(StackReserveBytes), \
      [cleanup] "i"(std::addressof(CleanUpFunc)), [record] "i"(std::addressof(RecordFull)),     \
      [fault] "i"(std::addressof(FaultFunc)), [entry] "i"(Detail::entryRegs<StackTop>)

        [[gnu::naked]] static void onIsrFull() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAVE_ENTRY
                           KVASIR_FAULT_PICK_FRAME KVASIR_FAULT_SAFE_SP
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_FULL_OPERANDS
                         : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "memory");
        }

        // For a non-primary stack on a core with MSPLIM (onIsrSecondary's reason): the limits are
        // saved first, then MSPLIM is cleared before SP moves.
        [[gnu::naked]] static void onIsrFullSecondary() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAVE_ENTRY KVASIR_FAULT_PICK_FRAME
                         "movs r0, #0        \n"
                         "msr msplim, r0     \n" KVASIR_FAULT_SAFE_SP
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[cleanup] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[fault] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_FULL_OPERANDS
                         : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "memory");
        }

#undef KVASIR_FAULT_FULL_OPERANDS
#undef KVASIR_FAULT_PICK_FRAME
#undef KVASIR_FAULT_SAVE_ENTRY
#undef KVASIR_FAULT_SAVE_LIMITS
#undef KVASIR_FAULT_SP_OPERANDS
#undef KVASIR_FAULT_SAFE_SP
#undef KVASIR_FAULT_ASM_BEGIN

        // Whether this image takes the Full entry points: the policy says Full, and the image logs
        // or Full asks to record without a log too.
        static constexpr bool fullRecord() {
            using Policy = CrashRecord::Detail::PolicyFor<StackTop>;
            if constexpr(CrashRecord::Detail::isFull<Policy>) {
#ifdef USE_UC_LOG
                return true;
#else
                return Policy::options.withoutLog;
#endif
            } else {
                return false;
            }
        }

        static constexpr auto getIsrFunc() {
            if constexpr(fullRecord()) {
                if constexpr(primaryStack || !hasStackLimit) {
                    return std::addressof(onIsrFull);
                } else {
                    return std::addressof(onIsrFullSecondary);
                }
            } else {
                return getLegacyIsrFunc();
            }
        }

        static constexpr auto getLegacyIsrFunc() {
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
