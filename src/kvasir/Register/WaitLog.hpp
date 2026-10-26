#pragma once
// The line of OnTimeout::Log (kvasir/Register/Wait.hpp): a Startup entry whose Hook::MainLoop turn writes one warning
// per batch of new timeouts, "register wait: N timeout(s), the last on the register at 0x...", at most once a turn.
// Apart from Wait.hpp, which the log's config header reads (it must not reach uc_log):
//
//     template<>
//     struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> { using type = Kvasir::Register::OnTimeout::Log; };
//     #include <kvasir/Register/WaitLog.hpp>
//     using Startup = Kvasir::Startup::Startup<..., Kvasir::Register::TimeoutReport, ...>;
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Register/Wait.hpp"

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

#include <cstdint>

namespace Kvasir::Register {
struct TimeoutReport {
    static inline std::uint32_t volatile reported{};

    static void turn() {
        std::uint32_t const n = OnTimeout::Log::timeouts;
        if(n == reported) { return; }
#ifdef USE_UC_LOG
        UC_LOG_W("register wait: {} timeout(s), the last on the register at 0x{:08x}",
                 n - reported,
                 static_cast<std::uint32_t>(OnTimeout::Log::lastAddress));
#endif
        reported = n;
    }

    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &turn>;
};
}   // namespace Kvasir::Register
