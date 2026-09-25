#pragma once

#include "cmake_git_version/version.hpp"
#include "kvasir/Util/FaultHandler.hpp"
#include "uc_log/uc_log.hpp"

/// Logs the build version, the reset cause and the fault the previous run ended in, if any.
///
///     Kvasir::Boot::logBoot(Kvasir::PM::reset_cause());
namespace Kvasir { namespace Boot {
    template<typename ResetCause>
    void logBoot(ResetCause cause) {
        UC_LOG_I("boot: {} reset cause: {}", CMakeGitVersion::FullVersion, cause);
        Fault::logLastFault();
    }
}}   // namespace Kvasir::Boot
