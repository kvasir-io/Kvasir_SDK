#pragma once

#include "cmake_git_version/version.hpp"
#include "kvasir/Util/FaultHandler.hpp"
#include "kvasir/Util/Panic.hpp"
#include "uc_log/uc_log.hpp"

#include <string_view>

namespace Kvasir { namespace Panic {
    /// One line if the run before this one ended in a panic (Panic.hpp's record).
    inline void logLastPanic() {
        [[maybe_unused]] auto const p = takeLastPanic();
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

/// Logs the build version, the reset cause and the fault or panic the previous run ended in, if any.
///
///     Kvasir::Boot::logBoot(Kvasir::PM::reset_cause());
namespace Kvasir { namespace Boot {
    template<typename ResetCause>
    void logBoot(ResetCause cause) {
        UC_LOG_I("boot: {} reset cause: {}", CMakeGitVersion::FullVersion, cause);
        Fault::logLastFault();
        Panic::logLastPanic();
    }
}}   // namespace Kvasir::Boot
