#pragma once

// The passkey a gated watchdog's feed takes (a chip watchdog Config with `gatedFeed = true`): only
// Kvasir::Health::Supervisor can make one, so nothing else in the image can feed that watchdog. And the shape of a
// watchdog the supervisor drives (WatchdogShape). Its own header, so a chip package does not include the whole
// supervisor.
#include <chrono>
#include <concepts>

namespace Kvasir::Health {
template<typename Startup, typename Watchdog, typename Clock, typename Config>
struct Supervisor;

class FeedKey {
    constexpr FeedKey() = default;

    template<typename Startup, typename Watchdog, typename Clock, typename Config>
    friend struct Supervisor;
};

/// A watchdog Health::Supervisor can drive: feed() - with its FeedKey when the watchdog is gated, without one when
/// not - and Timeout, the time from a feed to the reset as a std::chrono duration (the supervisor's period checks
/// compare with it). arm() and disarm() are optional (with or without the key the same way): a watchdog without
/// arm() is fed at the first turn, one without disarm() cannot be held off. The chip watchdogs:
/// Kvasir::Watchdog<Config> (RP, SAM, STM32), each on the one gated front (Health/WatchdogFront.hpp).
template<typename W>
concept WatchdogShape
  = (requires(FeedKey key) { W::feed(key); } || requires { W::feed(); })
 && requires {
        { W::timeout.count() } -> std::convertible_to<long long>;
        std::chrono::duration_cast<std::chrono::milliseconds>(W::timeout);
    };
}   // namespace Kvasir::Health
