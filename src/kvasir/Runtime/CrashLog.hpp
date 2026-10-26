#pragma once
// The log text of each crash record, written once: the lines the boot report prints for what the run before this
// one left behind (FaultRecord.hpp, Panic.hpp). Scripts and READMEs match these lines - the texts are fixed.
//
// One formatter per record, for code that takes a record itself and wants the boot report's line for it:
//
//     if(auto const p = Kvasir::Panic::takeLastPanic()) { Kvasir::Panic::logPanic(*p); }   // one line
//     if(auto const f = Kvasir::Fault::takeLastFault()) { Kvasir::Fault::logFault(*f); }   // three lines
//
// Without USE_UC_LOG every formatter is empty. The formatters keep an empty template pack: the application's log backend is specialised after the SDK headers and
// must be looked up where they are used.
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

#include <string_view>

namespace Kvasir { namespace Fault {
    /// "the run before this one ended in a fault on core N (...)": the whole state, three lines.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline void logFault([[maybe_unused]] FullRecord const& f) {
#ifdef USE_UC_LOG
        UC_LOG_E(
          "the run before this one ended in a fault on core {} ({} since the last report, this "
          "is the first): CFSR {:#010x} HFSR {:#010x} MMFAR {:#010x} BFAR {:#010x} SHCSR "
          "{:#010x} ICSR {:#010x} flags {:#06x}",
          (f.flags >> FullRecord::coreShift) & 0xFU,
          f.count,
          f.cfsr,
          f.hfsr,
          f.mmfar,
          f.bfar,
          f.shcsr,
          f.icsr,
          f.flags);
        UC_LOG_E(
          "  PC={:#010x} LR={:#010x} SP={:#010x} xPSR={:#010x} EXC_RETURN={:#010x} "
          "CONTROL={:#x} R0={:#x} R1={:#x} R2={:#x} R3={:#x} R12={:#x}",
          f.pc,
          f.lr,
          f.sp,
          f.xpsr,
          f.excReturn,
          f.control,
          f.r0,
          f.r1,
          f.r2,
          f.r3,
          f.r12);
        UC_LOG_E(
          "  R4={:#x} R5={:#x} R6={:#x} R7={:#x} R8={:#x} R9={:#x} R10={:#x} R11={:#x} "
          "MSP={:#010x} PSP={:#010x} MSPLIM={:#010x} PSPLIM={:#010x} SFSR={:#x} SFAR={:#x}",
          f.r4,
          f.r5,
          f.r6,
          f.r7,
          f.r8,
          f.r9,
          f.r10,
          f.r11,
          f.msp,
          f.psp,
          f.msplim,
          f.psplim,
          f.sfsr,
          f.sfar);
#endif
    }
}}   // namespace Kvasir::Fault

namespace Kvasir { namespace Panic {
    /// "the run before this one ended in a panic: <cause>, PC=..., detail ..., at ... us (...)", one line. The time
    /// is the chip's free-running microsecond counter when the panic was recorded (Port::FreeRunningUs; RP: the
    /// TIMER, counting since its release in that boot); a chip without one prints no time.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline void logPanic([[maybe_unused]] FullRecord const& p) {
#ifdef USE_UC_LOG
        if constexpr(Detail::hasFreeRunningUs<Dummy...>) {
            UC_LOG_E(
              "the run before this one ended in a panic: {}, PC={:#010x}, detail {}, at {} us ({} "
              "since the last report)",
              std::string_view{name(static_cast<Cause>(p.cause))},
              p.pc,
              p.detail,
              p.timeUs(),
              p.count);
        } else {
            UC_LOG_E(
              "the run before this one ended in a panic: {}, PC={:#010x}, detail {} ({} since the "
              "last report)",
              std::string_view{name(static_cast<Cause>(p.cause))},
              p.pc,
              p.detail,
              p.count);
        }
#endif
    }
}}   // namespace Kvasir::Panic
