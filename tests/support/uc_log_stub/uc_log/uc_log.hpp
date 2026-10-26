#pragma once
// A stand-in for uc_log's uc_log.hpp in host tests built with USE_UC_LOG but without the uc_log submodule: each log
// macro records its level letter into stub::events and takes its arguments (a stub that dropped them would leave
// locals used only in log lines unused, -Werror). No Runtime.hpp here, so Panic.hpp leaves the panic mode out.
#include <string>

namespace stub {
inline constexpr bool isStub = true;
inline std::string    events;

template<typename... Args>
void record(char level,
            Args const&...) {
    events += level;
}
}   // namespace stub

#define UC_LOG_T(...) ::stub::record('T', __VA_ARGS__)
#define UC_LOG_D(...) ::stub::record('D', __VA_ARGS__)
#define UC_LOG_I(...) ::stub::record('I', __VA_ARGS__)
#define UC_LOG_W(...) ::stub::record('W', __VA_ARGS__)
#define UC_LOG_E(...) ::stub::record('E', __VA_ARGS__)
#define UC_LOG_C(...) ::stub::record('C', __VA_ARGS__)
#define UC_LOG_SCOPE_MODULE(name)
