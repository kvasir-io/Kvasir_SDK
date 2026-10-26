#pragma once
#include "core/Fault.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/LinkerSymbols.hpp"
#include "kvasir/Register/Memory.hpp"
#include "kvasir/Runtime/CrashLog.hpp"
#include "kvasir/Runtime/CrashRecord.hpp"
#include "kvasir/Runtime/FaultReaction.hpp"
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/FullRecordBuild.hpp"
#include "kvasir/Runtime/Panic.hpp"
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
    namespace Detail {
        extern "C" {
        // the image's code and RAM (linker/common*.ld, every Kvasir image has them)
        extern char const _LINKER_INTERN_rom_start_[];
        extern char const _LINKER_INTERN_rom_end_[];
        extern char const _LINKER_INTERN_ram_start_[];
        extern char const _LINKER_INTERN_ram_end_[];
        }

        // Whether `bytes` from `at` lie inside the image's RAM: the only memory the fault record
        // reads through a pointer it got from the faulting context (besides the handler's own stack).
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
            return (Kvasir::Memory::load<std::uint16_t>(at) & 0xFF00U) == 0xBE00U;
        }

        // Whether the 8-word exception frame at `at` can be read: inside the image's RAM, or - for a handler on a
        // secondary stack - inside that stack, which may lie outside `ram` (a core 1 stack in a scratch bank,
        // CORE1_STACK_PLACEMENT scratch). A frame anywhere else came from a wild SP, and reading it would fault
        // again inside the fault handler: at priority -1 that is a lockup (Armv6-M ARM DDI0419E B1.5.15, Table
        // B1-7 "Memory Fault, Precise Data"), at a configurable priority it escalates to a HardFault, which then
        // records the nested fault instead of this one.
        template<typename StackTop>
        inline bool frameReadable(std::uintptr_t at) {
            if constexpr(std::is_same_v<StackTop, Startup::PrimaryStackTop>) {
                return inRam(static_cast<std::uint32_t>(at), 8 * 4);
            } else {
                auto const bottom = reinterpret_cast<std::uintptr_t>(StackTop::bottom);
                auto const top    = reinterpret_cast<std::uintptr_t>(StackTop::top);
                return inRam(static_cast<std::uint32_t>(at), 8 * 4)
                    || ((at & 3U) == 0 && at >= bottom && at <= top && 8 * 4 <= top - at);
            }
        }

        // MSTKERR, STKERR and (Armv8-M) STKOF: the frame was not (wholly) pushed. The masks are the core's
        // decode-table rows (core/Fault.hpp faultTable, from its SVD's MMFSR/BFSR/UFSR fields; Armv8-M ARM
        // DDI0553B.y D1.2.167, D1.2.7, D1.2.270): ExceptionStackingError and StackOverflow. Armv6-M has no CFSR
        // (Core::snapshot() gives 0): the range check is all there is.
#if KVASIR_CORE_HAS_FAULT_STATUS
        inline constexpr std::uint32_t stackingErrors = [] {
            std::uint32_t mask = 0;
            auto const    add  = [&mask](auto const& rows) {
                for(auto const& row : rows) {
                    if(row.what == Core::FaultDescription::exceptionStackingError
                       || row.what == Core::FaultDescription::stackOverflow)
                    {
                        mask |= row.mask;
                    }
                }
            };
            add(Core::faultTable.memManage);
            add(Core::faultTable.bus);
            add(Core::faultTable.usage);
            return mask;
        }();
#else
        inline constexpr std::uint32_t stackingErrors = 0;
#endif

        // What the entry points save before anything else runs, per handler (so per core):
        // r4-r11, EXC_RETURN, MSP, PSP, CONTROL, MSPLIM, PSPLIM. A fixed .bss address costs no
        // stack, which matters when the fault is a stack overflow.
        template<typename StackTop>
        inline std::uint32_t entryRegs[entryWords];   // the indexes: FullRecordBuild.hpp

        // The clean-ups HandlerFor chooses between (Options::cleanUp): nothing, or the image's fatal start.
        struct NoCleanUp {
            void operator()() {}
        };

        // What beginFatal() runs first, the image's Panic::FatalStartReaction (its safe state,
        // kvasir/StartUp/SafeState.hpp). An image without one has no clean-up.
        struct FatalStartCleanUp {
            template<typename... Dummy>
                requires(sizeof...(Dummy) == 0)
            void operator()() const {
                Panic::Detail::runFatalStart<Dummy...>();
            }
        };
    }   // namespace Detail

    // A HandlerFor's choices by name: `Fault::HandlerFor<{.reserveBytes = 2048, .cleanUp = false}, Core1StackTop>`.
    struct Options {
        // The top of the faulting stack the handler leaves alone (a multiple of 8; below).
        std::size_t reserveBytes = 16384;
        // Run the image's fatal start (Panic::FatalStartReaction, its safe state) on the fault stack after the
        // record; false: nothing besides the record.
        bool cleanUp = true;
    };

    // The fault handler, a Startup entry: records the fault (CrashRecord::RecordFormat's), runs the clean-up
    // (Options::cleanUp), then ends in the image's FaultReaction (default OnFault::Panic: the panic path, then the
    // image's Panic::PanicReaction - by default the chip's reboot).
    //
    // StackTop: which stack the handler runs on, Startup::PrimaryStackTop (the boot core's,
    // the default and the only choice on a single-core chip) or Startup::Core1StackTop for
    // the handler in a SecondaryCore's list. The handler sets SP to StackTop minus
    // reserveBytes and runs down from there, so the top reserveBytes of the
    // faulting context survive untouched and are what Core::Log (the core package's Fault::Core) reads its frame
    // from; a context that had grown deeper than the reserve has its frame under the
    // handler's own pushes. So the reserve wants to be more than the stack's expected
    // high-water mark and less than the stack.
    //
    // The stack's size is only known after the link (the boot stack takes whatever RAM is
    // left), so the handler caps the reserve at run time: it never goes below the middle of
    // the stack (rounded down to 8 bytes), and the handler always has the lower half of it.
    // Without the cap the 16 KB default would put SP below RAM on a 16 KB part, and the first
    // push would lock the core up without a fault record.
    template<Options Opts = Options{}, typename StackTop = Startup::PrimaryStackTop>
    struct HandlerFor {
        static constexpr std::size_t reserveBytes = Opts.reserveBytes;
        using CleanUp
          = std::conditional_t<Opts.cleanUp, Detail::FatalStartCleanUp, Detail::NoCleanUp>;
        static constexpr bool primaryStack = std::is_same_v<StackTop, Startup::PrimaryStackTop>;

        // AAPCS: SP 8-byte aligned at every call the handler makes; the stack's ends are
        // (linker/common_stack_body.inc.ld).
        static_assert(reserveBytes % 8 == 0,
                      "Fault::Options::reserveBytes must be a multiple of 8");

#ifdef KVASIR_CORE1_STACK_BYTES
        // util.cmake's CORE1_STACK_SIZE in bytes: the default 16 KB reserve is more than any
        // core 1 stack in use (and 4x the RP2040's scratch-bank one); the run-time cap would
        // halve it, but a core 1 handler is written for its stack and should say what it wants.
        static_assert(primaryStack || reserveBytes < KVASIR_CORE1_STACK_BYTES,
                      "Fault::HandlerFor on Core1StackTop: reserveBytes must be less than "
                      "CORE1_STACK_SIZE (and more than core 1's stack high-water mark)");
#endif

        // Whether the core has a stack limit register (MSPLIM: Armv8-M). Without one the
        // secondary-stack entry points below have nothing to clear, and the primary ones serve
        // both stacks (the asm takes StackTop's symbols).
        static constexpr bool hasStackLimit = KVASIR_CORE_HAS_STACK_LIMITS != 0;

        // Startup: the handler for a core's stack belongs in that core's list, or its fault
        // stack lands in the other core's region (kvasir/StartUp/Resources.hpp, rule 5).
        static constexpr unsigned startupCore = primaryStack ? 0 : 1;

        static void cleanUp() { CleanUp{}(); }

        // The image's FaultReaction, looked up where the entry is instantiated (StackTop is dependent).
        static void react() { typename Port::LateT<FaultReaction<Tag::User>, StackTop>::type{}(); }

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

        // The whole fault state into Fault::lastFault (and the stack into lastStack), called by
        // the entry points on the fault stack BEFORE the clean-up action (record first:
        // the clean-up must not change what is recorded). Reads nothing through a pointer from
        // the faulting context that is not inside RAM or the handler's own stack
        // (Detail::frameReadable), so it cannot fault again here.
        static void record(std::uint32_t const* frame,
                           std::uint32_t        excReturn) {
            using Policy       = CrashRecord::Records<StackTop>;
            constexpr auto o   = Policy::options;
            auto const     sys = Core::snapshot();
            auto const&    e   = Detail::entryRegs<StackTop>;
            auto const     at  = reinterpret_cast<std::uintptr_t>(frame);
            // a stacking error (Detail::stackingErrors) or a frame outside readable memory: no frame
            bool const frameOk
              = (sys.cfsr & Detail::stackingErrors) == 0 && Detail::frameReadable<StackTop>(at);

            // A panic ends on a bkpt (Panic::halt) under OnPanic::Halt. Without a debugger, and on the RP2040 also
            // when a debugger resets a core that sits on it, the bkpt escalates to a HardFault: that is the panic's
            // own stop, already named by Panic::lastPanic, not a second failure - no fault record for it, so the
            // next boot does not print "ended in a fault" next to the panic. Only a readable frame can show that
            // it was the bkpt.
            if(Policy::Panic::recorded() && frameOk && Detail::isBkpt(frame[6])) {
                logRecord(frame, excReturn, sys, frameOk);
                Core::clearFaultStatus(sys.cfsr, sys.hfsr);
                return;
            }
            // keep the first fault: later ones before the next boot are its consequences
            if(auto old = lastFault.load()) {
                old->count = old->count + 1;
                lastFault.store(*old);
                logRecord(frame, excReturn, sys, frameOk);
                Core::clearFaultStatus(sys.cfsr, sys.hfsr);
                return;
            }

            FullRecord r = Detail::buildFullRecord(
              frame,
              frameOk,
              at,
              excReturn,
              e,
              sys,
              Detail::RecordContext{.arch        = static_cast<std::uint32_t>(__ARM_ARCH),
                                    .core        = startupCore,
                                    .stackLimits = KVASIR_CORE_ARMV8M_MAIN != 0});

            if constexpr(o.stackBytes != 0) {
                // the SP the frame went to; without a valid frame (an overflow) the one EXC_RETURN.SPSEL
                // names, as the core left it. Copied only from inside `ram`: a stack outside it (a core 1
                // stack in a scratch bank) gets a snapshot of 0 bytes.
                std::uint32_t const from = Detail::stackCopyFrom(r, excReturn);
                std::uint32_t const n    = Detail::stackCopyBytes(
                  from,
                  Detail::inRam(from, 0),
                  reinterpret_cast<std::uintptr_t>(Detail::_LINKER_INTERN_ram_end_),
                  o.stackBytes);
                lastStack<o.stackBytes>.storeWords([from, n](std::size_t i) -> std::uint32_t {
                    if(i == 0) { return from; }
                    if(i == 1) { return n; }
                    auto const offset = static_cast<std::uint32_t>((i - 2) * 4);
                    return offset < n ? Kvasir::Memory::load<std::uint32_t>(from + offset) : 0U;
                });
                r.flags |= FullRecord::stackCopied;
            }
            lastFault.store(r);
            logRecord(frame, excReturn, sys, frameOk);
            Core::clearFaultStatus(sys.cfsr, sys.hfsr);
        }

        static void logRecord([[maybe_unused]] std::uint32_t const*  frame,
                              [[maybe_unused]] std::uint32_t         excReturn,
                              [[maybe_unused]] Core::Snapshot const& sys,
                              [[maybe_unused]] bool                  frameOk) {
#ifdef USE_UC_LOG
            if(frameOk) {
                // fatal end: the fault handler's action after the dump; the dump's lines wait for room
                Panic::beginFatal();
                Core::log(frame, excReturn, sys);   // decodes the snapshot record() took
            }
#endif
        }

// The entry points save r4-r11, EXC_RETURN, MSP, PSP, CONTROL (and on Armv8-M main MSPLIM,
// PSPLIM) into Detail::entryRegs<StackTop> first, before the sequence below touches r4-r7; r0-r3
// and r12 are in the stacked frame already. Same layout on both: words 0-7 r4-r11, 8 EXC_RETURN,
// 9 MSP, 10 PSP, 11 CONTROL, 12 MSPLIM, 13 PSPLIM. Then record(), then the clean-up,
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
    #if KVASIR_CORE_ARMV8M_MAIN
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

#define KVASIR_FAULT_OPERANDS                                                              \
    [top] "i"(StackTop::top), [bottom] "i"(StackTop::bottom), [reserve] "n"(reserveBytes), \
      [cleanUp] "i"(std::addressof(cleanUp)), [record] "i"(std::addressof(record)),        \
      [react] "i"(std::addressof(react)), [entry] "i"(Detail::entryRegs<StackTop>)

        [[gnu::naked]] static void onIsr() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAVE_ENTRY
                           KVASIR_FAULT_PICK_FRAME KVASIR_FAULT_SAFE_SP
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[cleanUp] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[react] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_OPERANDS
                         : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "memory");
        }

        // For a non-primary stack on a core with MSPLIM, never instantiated on a core without one
        // (hasStackLimit): the limits are saved first, then MSPLIM is cleared before SP moves. The
        // handler's reserve sits below the stack limit the core runs with, so with the limit still
        // armed the first push in here would raise a second stack-overflow fault inside the fault
        // handler, which is a lockup.
        [[gnu::naked]] static void onIsrSecondary() {
            asm volatile(KVASIR_FAULT_ASM_BEGIN KVASIR_FAULT_SAVE_ENTRY KVASIR_FAULT_PICK_FRAME
                         "movs r0, #0        \n"
                         "msr msplim, r0     \n" KVASIR_FAULT_SAFE_SP
                         "mov r0, r5         \n"
                         "mov r1, r4         \n"
                         "bl %c[record]      \n"
                         "ldr r6, =%c[cleanUp] \n"
                         "blx r6             \n"
                         "ldr r6, =%c[react] \n"
                         "blx r6             \n"
                         ".ltorg             \n"
                         :
                         : KVASIR_FAULT_OPERANDS
                         : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "memory");
        }

#undef KVASIR_FAULT_OPERANDS
#undef KVASIR_FAULT_PICK_FRAME
#undef KVASIR_FAULT_SAVE_ENTRY
#undef KVASIR_FAULT_SAVE_LIMITS
#undef KVASIR_FAULT_SAFE_SP
#undef KVASIR_FAULT_ASM_BEGIN

        static constexpr auto getIsrFunc() {
            if constexpr(primaryStack || !hasStackLimit) {
                return std::addressof(onIsr);
            } else {
                return std::addressof(onIsrSecondary);
            }
        }

        static constexpr auto earlyInit = Core::EarlyInitList{};

        // the chip's traits looked up late (dependent on StackTop): this header may come before the chip's
        using Isr
          = Nvic::IsrsFor<getIsrFunc(),
                          typename Port::LateT<Nvic::InterruptOffsetTraits<Kvasir::Tag::User>,
                                               StackTop>::FaultInterruptIndexs>;
    };
}}   // namespace Kvasir::Fault
