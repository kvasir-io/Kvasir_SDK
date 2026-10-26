#pragma once
// The fault record built from what the fault handler gathered (FaultHandler.hpp HandlerFor::record): the stacked
// frame, the registers its entry point saved, the core's fault-register snapshot and EXC_RETURN. Register-free and
// without a core package (the snapshot is a template parameter with the members of cortex_common/FaultSnapshot.hpp),
// so every flag and field is host-tested (tests/full_record_build_tests.cpp); the handler keeps the reads and the
// range checks that decide what may be read.
#include "kvasir/Runtime/FaultRecord.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Kvasir { namespace Fault { namespace Detail {
    // What the entry points save before anything else runs, per handler (so per core): r4-r11, EXC_RETURN, MSP,
    // PSP, CONTROL, MSPLIM, PSPLIM (FaultHandler.hpp entryRegs).
    inline constexpr std::size_t entryWords     = 14;
    inline constexpr std::size_t entryExcReturn = 8, entryMsp = 9, entryPsp = 10, entryControl = 11,
                                 entryMsplim = 12, entryPsplim = 13;

    // What the image adds to a record: the architecture (__ARM_ARCH), the Startup core of the handler (0, 1 = the
    // secondary stack) and whether the entry saved MSPLIM/PSPLIM (Armv8-M main).
    struct RecordContext {
        std::uint32_t arch;
        std::uint32_t core;
        bool          stackLimits;
    };

    // The record of a first fault: count 1, flags, the frame (only when `frameOk`: the handler checked it can be read
    // at `at`), the entry registers, the snapshot. `sp` is the faulting context's SP before the frame: 8 words, 26
    // with the FP state (EXC_RETURN.FType 0), one more when the processor aligned the stack (stacked xPSR bit 9;
    // Armv7-M ARM DDI0403E.e B1.5.7); 0 without a frame.
    template<typename Snapshot>
    [[gnu::always_inline]] inline FullRecord buildFullRecord(std::uint32_t const* frame,
                                                             bool                 frameOk,
                                                             std::uintptr_t       at,
                                                             std::uint32_t        excReturn,
                                                             std::uint32_t const (&e)[entryWords],
                                                             Snapshot const& sys,
                                                             RecordContext   ctx) {
        FullRecord r{};
        r.count = 1;
        r.flags = (ctx.arch << FullRecord::archShift) | (ctx.core << FullRecord::coreShift)
                | FullRecord::calleeSaved
                | (sys.faultRegs ? std::uint32_t{FullRecord::faultRegs} : 0U)
                | (sys.secureRegs ? std::uint32_t{FullRecord::secureRegs} : 0U)
                | ((excReturn & (1U << 4)) == 0 ? std::uint32_t{FullRecord::fpFrame} : 0U);
        if(frameOk) {
            r.r0                      = frame[0];
            r.r1                      = frame[1];
            r.r2                      = frame[2];
            r.r3                      = frame[3];
            r.r12                     = frame[4];
            r.lr                      = frame[5];
            r.pc                      = frame[6];
            r.xpsr                    = frame[7];
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
        r.msp       = e[entryMsp];
        r.psp       = e[entryPsp];
        r.control   = e[entryControl];
        if(ctx.stackLimits) {
            r.msplim = e[entryMsplim];
            r.psplim = e[entryPsplim];
            r.flags |= FullRecord::stackLimits;
        }
        r.cfsr  = sys.cfsr;
        r.hfsr  = sys.hfsr;
        r.mmfar = sys.mmfar;
        r.bfar  = sys.bfar;
        r.shcsr = sys.shcsr;
        r.icsr  = sys.icsr;
        r.sfsr  = sys.sfsr;
        r.sfar  = sys.sfar;
        return r;
    }

    // Where the stack copy starts: the SP the frame went to; without a valid frame (an overflow) the one
    // EXC_RETURN.SPSEL names (bit 2: PSP), as the core left it. Word-aligned down.
    [[gnu::always_inline]] inline std::uint32_t stackCopyFrom(FullRecord const& r,
                                                              std::uint32_t     excReturn) {
        return (r.sp != 0 ? r.sp : ((excReturn & (1U << 2)) != 0 ? r.psp : r.msp)) & ~3U;
    }

    // How many bytes of it are copied: at most `stackBytes`, only from inside RAM (`fromInRam`, up to `ramEnd`), else
    // 0 (a stack outside RAM, a core 1 stack in a scratch bank, gets a snapshot of 0 bytes).
    [[gnu::always_inline]] inline std::uint32_t stackCopyBytes(std::uint32_t  from,
                                                               bool           fromInRam,
                                                               std::uintptr_t ramEnd,
                                                               std::uint32_t  stackBytes) {
        return fromInRam
               ? std::min<std::uint32_t>(stackBytes, static_cast<std::uint32_t>(ramEnd - from))
               : 0U;
    }
}}}   // namespace Kvasir::Fault::Detail
