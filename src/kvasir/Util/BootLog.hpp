#pragma once

#include "cmake_git_version/version.hpp"
#include "kvasir/Util/CrashRecord.hpp"
#include "kvasir/Util/FaultHandler.hpp"
#include "kvasir/Util/Panic.hpp"
#include "uc_log/uc_log.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Kvasir { namespace Panic {
    /// One line if the run before this one ended in a panic (Panic.hpp's record).
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline void logLastPanic() {
        [[maybe_unused]] auto const p = takeLastPanic<Dummy...>();
        if(p) {
            UC_LOG_E(
              "the run before this one ended in a panic: {}, PC={:#010x}, detail {} ({} since the "
              "last report)",
              std::string_view{name(static_cast<Cause>(p->cause))},
              p->pc,
              p->detail,
              p->count);
        }
    }
}}   // namespace Kvasir::Panic

/// The version, the reset cause and the fault or panic the previous run ended in, if any:
///
///     Kvasir::Boot::logBoot(Kvasir::PM::reset_cause());
///
/// Or in two steps, for code that must see the records too (a boot-loop guard) - takePrevious()
/// takes them once, and drops them unread after a power-on (CrashRecord::Options::clearOnPowerOn):
///
///     auto const previous = Kvasir::Boot::takePrevious(powerOn);
///     Kvasir::Boot::logBoot(Kvasir::PM::reset_cause(), previous);
namespace Kvasir { namespace Boot {
    /// What the run before this one left behind. `fault` and `panic` have the same shape under
    /// either CrashRecord policy; `fullFault` is the whole state, under Full only.
    struct Previous {
        std::optional<Fault::Record>     fault;
        std::optional<Fault::FullRecord> fullFault;
        std::optional<Panic::Record>     panic;
    };

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    Previous takePrevious(bool powerOn = false) {
        using Policy = std::remove_cvref_t<decltype(CrashRecord::injectedPolicy<Dummy...>)>;
        Previous p{};
        if constexpr(CrashRecord::Detail::isFull<Policy>) {
            p.fullFault = Fault::takeLastFullFault();
            if(p.fullFault) {
                auto const& f = *p.fullFault;
                p.fault       = Fault::Record{.magic     = Fault::Record::Magic,
                                              .count     = f.count,
                                              .pc        = f.pc,
                                              .lr        = f.lr,
                                              .xpsr      = f.xpsr,
                                              .excReturn = f.excReturn,
                                              .r0        = f.r0,
                                              .r1        = f.r1,
                                              .r2        = f.r2,
                                              .r3        = f.r3,
                                              .r12       = f.r12};
            }
            p.panic = Panic::takeLastPanic<Dummy...>();
            if(powerOn && Policy::options.clearOnPowerOn) { return Previous{}; }
        } else {
            p.fault = Fault::takeLastFault<Dummy...>();
            p.panic = Panic::takeLastPanic<Dummy...>();
        }
        return p;
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    void logPrevious([[maybe_unused]] Previous const& p) {
        if(p.fullFault) {
            [[maybe_unused]] auto const& f = *p.fullFault;
            UC_LOG_E(
              "the run before this one ended in a fault on core {} ({} since the last report, this "
              "is the first): CFSR {:#010x} HFSR {:#010x} MMFAR {:#010x} BFAR {:#010x} SHCSR "
              "{:#010x} ICSR {:#010x} flags {:#06x}",
              (f.flags >> Fault::FullRecord::coreShift) & 0xFU,
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
        } else if(p.fault) {
            [[maybe_unused]] auto const& fault = *p.fault;
            UC_LOG_E(
              "the run before this one ended in a fault ({} since the last report, this is the "
              "first): PC={:#010x} LR={:#010x} xPSR={:#010x} EXC_RETURN={:#010x} R0={:#010x} "
              "R1={:#010x} R2={:#010x} R3={:#010x} R12={:#010x}",
              fault.count,
              fault.pc,
              fault.lr,
              fault.xpsr,
              fault.excReturn,
              fault.r0,
              fault.r1,
              fault.r2,
              fault.r3,
              fault.r12);
        }
        if(p.panic) {
            [[maybe_unused]] auto const& panic = *p.panic;
            UC_LOG_E(
              "the run before this one ended in a panic: {}, PC={:#010x}, detail {} ({} since the "
              "last report)",
              std::string_view{Panic::name(static_cast<Panic::Cause>(panic.cause))},
              panic.pc,
              panic.detail,
              panic.count);
        }
    }

    template<typename ResetCause>
    void logBoot(ResetCause      cause,
                 Previous const& previous) {
        UC_LOG_I("boot: {} reset cause: {}", CMakeGitVersion::FullVersion, cause);
        logPrevious(previous);
    }

    template<typename ResetCause>
    void logBoot(ResetCause cause) {
        using Policy = CrashRecord::Detail::PolicyFor<ResetCause>;
        if constexpr(CrashRecord::Detail::isFull<Policy>) {
            logBoot(cause, takePrevious());
        } else {
            // the lines and the order of before Boot::Previous existed
            UC_LOG_I("boot: {} reset cause: {}", CMakeGitVersion::FullVersion, cause);
            Fault::logLastFault();
            Panic::logLastPanic();
        }
    }
}}   // namespace Kvasir::Boot
