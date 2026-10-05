#pragma once

// The passkey a gated watchdog's feed takes (a chip watchdog Config with `gatedFeed = true`): only
// Kvasir::Health::Supervisor can make one, so nothing else in the image can feed that watchdog. Its own header, so a
// chip package does not include the whole supervisor.
namespace Kvasir::Health {
template<typename Startup, typename Watchdog, typename Clock, typename Config>
struct Supervisor;

class FeedKey {
    constexpr FeedKey() = default;

    template<typename Startup, typename Watchdog, typename Clock, typename Config>
    friend struct Supervisor;
};
}   // namespace Kvasir::Health
