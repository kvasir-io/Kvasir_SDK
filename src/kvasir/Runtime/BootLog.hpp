#pragma once

#include "cmake_git_version/version.hpp"
#include "kvasir/Runtime/CrashLog.hpp"
#include "kvasir/Runtime/CrashRecord.hpp"
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "uc_log/uc_log.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

/// The version, the reset cause and the fault or panic the previous run ended in, if any:
///
///     Kvasir::Boot::logBoot(Kvasir::PM::resetCause());
///
/// Or in two steps, for code that must see the records too (a boot-loop guard) - takePrevious()
/// takes them once, and drops them unread after a power-on (CrashRecord::Options::clearOnPowerOn):
///
///     auto const previous = Kvasir::Boot::takePrevious(powerOn);
///     Kvasir::Boot::logBoot(Kvasir::PM::resetCause(), previous);
namespace Kvasir { namespace Boot {
    /// What the run before this one left behind: the fault and the panic record.
    struct Previous {
        std::optional<Fault::FullRecord> fault;
        std::optional<Panic::FullRecord> panic;
    };

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    Previous takePrevious(bool powerOn = false) {
        using R = CrashRecord::Records<Previous, Dummy...>;
        Previous p{};
        p.fault = R::Fault::take();
        p.panic = Panic::takeLastPanic<Dummy...>();
        if(powerOn && R::Fault::clearOnPowerOn) { return Previous{}; }
        return p;
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    void logPrevious([[maybe_unused]] Previous const& p) {
        if(p.fault) { Fault::logFault<Dummy...>(*p.fault); }
        if(p.panic) { Panic::logPanic<Dummy...>(*p.panic); }
    }

    /// One more line when the chip package can tell that this start was not the reset the cause names: a chip
    /// whose start code resets the chip once when it was entered without a reset (a debugger's or the image's own
    /// jump) has the late hook Port::RestartedAfterJump<Tag::User> (kvasir/Port/Hooks.hpp). Then the chip's own
    /// lines about this start, Port::StartNotes<Tag::User> (an RP boot that runs degraded on the ring oscillator).
    /// Without either this is nothing.
    template<typename ResetCause>
    void logStartKind([[maybe_unused]] ResetCause cause) {
        using Restarted = Port::LateT<Port::RestartedAfterJump<Tag::User>, ResetCause>;
        if constexpr(requires { sizeof(Restarted); }) {
            if(Restarted{}()) {
                UC_LOG_W(
                  "boot: restarted after a jump: the image was entered without a reset and reset "
                  "the chip itself (the reset cause above is that reset)");
            }
        }
        using Notes = Port::LateT<Port::StartNotes<Tag::User>, ResetCause>;
        if constexpr(requires { sizeof(Notes); }) { Notes{}(); }
    }

    /// The run before this one left a fault or panic record: it ended in a failure.
    [[nodiscard]] constexpr bool endedInFailure(Previous const& p) {
        return p.fault.has_value() || p.panic.has_value();
    }

    /// The boot line: a routine boot is DEBUG, one after a run that ended in a fault or panic WARN (the records
    /// themselves follow at their own level). BootGuard's logReport keeps the same rule.
    template<typename ResetCause>
    void logBoot(ResetCause      cause,
                 Previous const& previous) {
        if(endedInFailure(previous)) {
            UC_LOG_W("boot: {} reset cause: {}", CMakeGitVersion::fullVersion, cause);
        } else {
            UC_LOG_D("boot: {} reset cause: {}", CMakeGitVersion::fullVersion, cause);
        }
        logStartKind(cause);
        logPrevious(previous);
    }

    template<typename ResetCause>
    void logBoot(ResetCause cause) {
        logBoot(cause, takePrevious());
    }
}}   // namespace Kvasir::Boot
