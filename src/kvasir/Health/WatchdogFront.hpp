#pragma once

// The one front of every chip watchdog (RP, SAM, STM32 `Kvasir::Watchdog<Config>`): arm(), feed() and disarm(), each
// with the supervisor's FeedKey when the watchdog is gated (Config `gatedFeed = true`) and without one when it is not.
// The chip's watchdog derives from it and gives the bodies as private `armNow()`, `feedNow()` and, where the hardware
// can stop the counter, `disarmNow()`, with this front as a friend:
//
//     template<typename Config>
//     struct Watchdog : Health::WatchdogFront<Watchdog<Config>, Health::gatedFeedOf<Config>()> {
//     private:
//         friend struct Health::WatchdogFront<Watchdog, Health::gatedFeedOf<Config>()>;
//         static void armNow();
//         static void feedNow();
//         static void disarmNow();   // optional: without it there is no disarm(), and the supervisor cannot hold off
//     };
//
// Each member is one always-inlined call of the body. Only the standard library and HealthKey.hpp: a chip package
// includes it without the supervisor.
#include "kvasir/Health/HealthKey.hpp"

namespace Kvasir::Health {
/// A watchdog Config's `gatedFeed` (default false).
template<typename Config>
consteval bool gatedFeedOf() {
    if constexpr(requires { Config::gatedFeed; }) {
        return static_cast<bool>(Config::gatedFeed);
    } else {
        return false;
    }
}

template<typename Hw, bool Gated>
struct WatchdogFront {
    /// For the list rule WatchdogBeforeHealthService (StartUp/ListRules.hpp): a watchdog in a Startup list comes
    /// before Health::Service<>, whose runtimeInit holds it off.
    static constexpr bool isWatchdog = true;
    /// A gated watchdog has no runtimeInit (the supervisor's first service() arms it): the rule lets it come after.
    static constexpr bool isGatedWatchdog = Gated;

    /// Arm at run time, as the Startup list would: for a watchdog that is not in the list, or one that was disarmed.
    [[gnu::always_inline]] static void arm()
        requires(!Gated)
    {
        Hw::armNow();
    }

    /// The gated arm: Supervisor::service() does it at its first turn.
    [[gnu::always_inline]] static void arm(FeedKey)
        requires(Gated)
    {
        Hw::armNow();
    }

    /// Restart the countdown at the full timeout.
    [[gnu::always_inline]] static void feed()
        requires(!Gated)
    {
        Hw::feedNow();
    }

    /// The gated feed: only the health supervisor holds a key.
    [[gnu::always_inline]] static void feed(FeedKey)
        requires(Gated)
    {
        Hw::feedNow();
    }

    /// Stop the countdown; only where the hardware can (the STM32 IWDG cannot).
    [[gnu::always_inline]] static void disarm()
        requires(!Gated && requires { Hw::disarmNow(); })
    {
        Hw::disarmNow();
    }

    /// The gated disarm: Supervisor::holdOff(), before its first turn only.
    [[gnu::always_inline]] static void disarm(FeedKey)
        requires(Gated && requires { Hw::disarmNow(); })
    {
        Hw::disarmNow();
    }
};
}   // namespace Kvasir::Health
