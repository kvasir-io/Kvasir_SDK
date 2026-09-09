#pragma once

/// The uc_log macros the device drivers and their base use without including uc_log
/// (the target TU has it). A host test counts them instead. Include before the driver.
namespace Kvasir::Test::Log {
inline int warnings{};
inline int infos{};
inline int debugs{};
inline int errors{};

inline void reset() { warnings = infos = debugs = errors = 0; }
}   // namespace Kvasir::Test::Log

#define UC_LOG_W(...) (++::Kvasir::Test::Log::warnings)
#define UC_LOG_I(...) (++::Kvasir::Test::Log::infos)
#define UC_LOG_D(...) (++::Kvasir::Test::Log::debugs)
#define UC_LOG_E(...) (++::Kvasir::Test::Log::errors)
#define KVASIR_LOG_LIMITED(decision, LOG, ...)               \
    do {                                                     \
        if(auto const d_ = (decision)) { LOG(__VA_ARGS__); } \
    } while(false)
