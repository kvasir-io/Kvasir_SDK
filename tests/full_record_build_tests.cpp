// The fault record's build (Runtime/FullRecordBuild.hpp), the code FaultHandler.hpp's record() runs on a first fault:
// every flag (core, architecture, calleeSaved, faultRegs, secureRegs, fpFrame, frameValid, stackLimits), the frame
// and entry registers in their fields, the SP before the frame (8 / 26 words, + 4 for the aligned stack), and the
// stack copy's start and length.
#include "kvasir/Runtime/FullRecordBuild.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>

namespace {
using Kvasir::Fault::FullRecord;
using Kvasir::Fault::Detail::buildFullRecord;
using Kvasir::Fault::Detail::stackCopyBytes;
using Kvasir::Fault::Detail::stackCopyFrom;

// cortex_common/FaultSnapshot.hpp's members (the SDK's tests have no core package)
struct Snapshot {
    std::uint32_t cfsr, hfsr, mmfar, bfar, shcsr, icsr, sfsr, sfar;
    bool          faultRegs;
    bool          secureRegs;
};

constexpr Snapshot v8 = {.cfsr       = 0x0010'0000U,
                         .hfsr       = 0x4000'0000U,
                         .mmfar      = 0x11U,
                         .bfar       = 0x22U,
                         .shcsr      = 0x7'0000U,
                         .icsr       = 0x803U,
                         .sfsr       = 0x51U,
                         .sfar       = 0x52U,
                         .faultRegs  = true,
                         .secureRegs = true};
constexpr Snapshot v6 = {.cfsr       = 0,
                         .hfsr       = 0,
                         .mmfar      = 0,
                         .bfar       = 0,
                         .shcsr      = 0,
                         .icsr       = 0x3U,
                         .sfsr       = 0,
                         .sfar       = 0,
                         .faultRegs  = false,
                         .secureRegs = false};

// r0-r3, r12, lr, pc, xpsr
constexpr std::array<std::uint32_t, 8>
  frame{0xA0, 0xA1, 0xA2, 0xA3, 0xAC, 0x1000'0101U, 0x1000'0200U, 0x0100'0000U};
// r4-r11, EXC_RETURN, MSP, PSP, CONTROL, MSPLIM, PSPLIM
constexpr std::uint32_t entry[14]{0xB4,
                                  0xB5,
                                  0xB6,
                                  0xB7,
                                  0xB8,
                                  0xB9,
                                  0xBA,
                                  0xBB,
                                  0xFFFF'FFF9U,
                                  0x2000'1000U,
                                  0x2000'2000U,
                                  0x2U,
                                  0x2000'0100U,
                                  0x2000'0200U};

constexpr std::uint32_t basicFrame
  = 0xFFFF'FFF9U;   // EXC_RETURN.FType (bit 4) 1: no FP state, MSP (bit 2 0)
constexpr std::uint32_t fpFrame   = 0xFFFF'FFE9U;   // FType 0: the extended frame
constexpr std::uint32_t pspReturn = 0xFFFF'FFFDU;   // SPSEL (bit 2) 1: PSP

constexpr std::uintptr_t at = 0x2000'0F00U;

void flags() {
    using Kvasir::Test::test;
    test("flags: Armv8-M main, core 1, every register set");
    auto const r = buildFullRecord(frame.data(),
                                   true,
                                   at,
                                   basicFrame,
                                   entry,
                                   v8,
                                   {.arch = 8, .core = 1, .stackLimits = true});
    CHECK_EQ(r.count, 1U);
    CHECK_EQ((r.flags >> FullRecord::archShift) & 0xFU, 8U);
    CHECK_EQ((r.flags >> FullRecord::coreShift) & 0xFU, 1U);
    CHECK_EQ(r.flags & 0xFFU,
             std::uint32_t{FullRecord::frameValid | FullRecord::calleeSaved | FullRecord::faultRegs
                           | FullRecord::stackLimits | FullRecord::secureRegs});

    test("flags: Armv6-M, core 0, no fault registers, no limits, no frame");
    auto const m0 = buildFullRecord(frame.data(),
                                    false,
                                    at,
                                    basicFrame,
                                    entry,
                                    v6,
                                    {.arch = 6, .core = 0, .stackLimits = false});
    CHECK_EQ((m0.flags >> FullRecord::archShift) & 0xFU, 6U);
    CHECK_EQ((m0.flags >> FullRecord::coreShift) & 0xFU, 0U);
    CHECK_EQ(m0.flags & 0xFFU, std::uint32_t{FullRecord::calleeSaved});

    test("flags: the FP frame");
    auto const fp = buildFullRecord(frame.data(),
                                    true,
                                    at,
                                    fpFrame,
                                    entry,
                                    v8,
                                    {.arch = 8, .core = 0, .stackLimits = true});
    CHECK((fp.flags & FullRecord::fpFrame) != 0U);
    CHECK((r.flags & FullRecord::fpFrame) == 0U);
    CHECK((m0.flags & FullRecord::stackCopied) == 0U);   // the handler sets it, after the copy
}

void fields() {
    using Kvasir::Test::test;
    test("fields: frame, entry registers, snapshot");
    auto const r = buildFullRecord(frame.data(),
                                   true,
                                   at,
                                   basicFrame,
                                   entry,
                                   v8,
                                   {.arch = 8, .core = 0, .stackLimits = true});
    CHECK_EQ(r.r0, 0xA0U);
    CHECK_EQ(r.r1, 0xA1U);
    CHECK_EQ(r.r2, 0xA2U);
    CHECK_EQ(r.r3, 0xA3U);
    CHECK_EQ(r.r12, 0xACU);
    CHECK_EQ(r.lr, 0x1000'0101U);
    CHECK_EQ(r.pc, 0x1000'0200U);
    CHECK_EQ(r.xpsr, 0x0100'0000U);
    CHECK_EQ(r.r4, 0xB4U);
    CHECK_EQ(r.r11, 0xBBU);
    CHECK_EQ(r.excReturn, basicFrame);
    CHECK_EQ(r.msp, 0x2000'1000U);
    CHECK_EQ(r.psp, 0x2000'2000U);
    CHECK_EQ(r.control, 0x2U);
    CHECK_EQ(r.msplim, 0x2000'0100U);
    CHECK_EQ(r.psplim, 0x2000'0200U);
    CHECK_EQ(r.cfsr, v8.cfsr);
    CHECK_EQ(r.hfsr, v8.hfsr);
    CHECK_EQ(r.mmfar, v8.mmfar);
    CHECK_EQ(r.bfar, v8.bfar);
    CHECK_EQ(r.shcsr, v8.shcsr);
    CHECK_EQ(r.icsr, v8.icsr);
    CHECK_EQ(r.sfsr, v8.sfsr);
    CHECK_EQ(r.sfar, v8.sfar);

    test("fields: no frame leaves the frame words and sp 0, no limits leaves msplim/psplim 0");
    auto const n = buildFullRecord(frame.data(),
                                   false,
                                   at,
                                   basicFrame,
                                   entry,
                                   v6,
                                   {.arch = 6, .core = 0, .stackLimits = false});
    CHECK_EQ(n.r0, 0U);
    CHECK_EQ(n.pc, 0U);
    CHECK_EQ(n.xpsr, 0U);
    CHECK_EQ(n.sp, 0U);
    CHECK_EQ(n.msplim, 0U);
    CHECK_EQ(n.psplim, 0U);
    CHECK_EQ(n.r4, 0xB4U);   // the entry registers are there anyway
}

void spBeforeTheFrame() {
    using Kvasir::Test::test;
    test("sp: 8 words, 26 with the FP state, + 4 when xPSR bit 9 says the stack was aligned");
    CHECK_EQ(buildFullRecord(frame.data(), true, at, basicFrame, entry, v8, {8, 0, true}).sp,
             at + 32U);
    CHECK_EQ(buildFullRecord(frame.data(), true, at, fpFrame, entry, v8, {8, 0, true}).sp,
             at + 104U);
    auto aligned = frame;
    aligned[7] |= 1U << 9;
    CHECK_EQ(buildFullRecord(aligned.data(), true, at, basicFrame, entry, v8, {8, 0, true}).sp,
             at + 36U);
    CHECK_EQ(buildFullRecord(aligned.data(), true, at, fpFrame, entry, v8, {8, 0, true}).sp,
             at + 108U);
}

void stackCopy() {
    using Kvasir::Test::test;
    test("stack copy: from the frame's sp, else the SP EXC_RETURN.SPSEL names, word-aligned");
    auto const withFrame
      = buildFullRecord(frame.data(), true, at, basicFrame, entry, v8, {8, 0, true});
    CHECK_EQ(stackCopyFrom(withFrame, basicFrame), static_cast<std::uint32_t>(at + 32U));
    auto noFrame = buildFullRecord(frame.data(), false, at, basicFrame, entry, v8, {8, 0, true});
    CHECK_EQ(stackCopyFrom(noFrame, basicFrame), 0x2000'1000U);   // MSP
    CHECK_EQ(stackCopyFrom(noFrame, pspReturn), 0x2000'2000U);    // PSP
    noFrame.msp = 0x2000'1003U;
    CHECK_EQ(stackCopyFrom(noFrame, basicFrame), 0x2000'1000U);

    test("stack copy: at most stackBytes, up to the end of RAM, nothing from outside it");
    CHECK_EQ(stackCopyBytes(0x2000'1000U, true, 0x2004'0000U, 256U), 256U);
    CHECK_EQ(stackCopyBytes(0x2003'FFC0U, true, 0x2004'0000U, 256U), 64U);
    CHECK_EQ(stackCopyBytes(0x2004'0000U, true, 0x2004'0000U, 256U), 0U);
    CHECK_EQ(stackCopyBytes(0x2008'0000U, false, 0x2004'0000U, 256U), 0U);
}
}   // namespace

int main() {
    flags();
    fields();
    spBeforeTheFrame();
    stackCopy();
    return Kvasir::Test::report();
}
