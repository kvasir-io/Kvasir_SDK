#pragma once
// A write that has not reached its target yet and is tried again by itself, with a pause that doubles after every
// failure up to a ceiling, and starts over after a success. A store keeps one and asks `due(now)` from its
// Hook::MainLoop function:
//
//     static inline Kvasir::Retry<Clock, Error> retry_{};
//     if(auto const r = attempt(); r) { retry_.succeeded(); } else { retry_.failed(r.error(), Clock::now()); }
//     ...
//     static void maintain() { if(retry_.due(Clock::now())) { (void)attempt(); ... } }
//
// A clock that jumps (a loop that was blocked for minutes) makes it due once, not once per missed pause.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace Kvasir {

struct RetryDefaults {
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::minutes longest{10};
};

template<typename Clock, typename Error, typename Cfg = RetryDefaults>
struct Retry {
    using duration   = typename Clock::duration;
    using time_point = typename Clock::time_point;

    static constexpr duration First   = std::chrono::duration_cast<duration>(Cfg::first);
    static constexpr duration Longest = std::chrono::duration_cast<duration>(Cfg::longest);
    static_assert(First > duration::zero() && First <= Longest,
                  "a retry pause must be positive and the first no longer than the longest");

    bool          pending{};      ///< a value that has not reached its target
    Error         last{};         ///< the error of the last failed attempt
    std::uint16_t failures{};     ///< since the last success
    duration      pause{First};   ///< the wait after the next failure
    time_point    next{};         ///< pending: the earliest next attempt

    void failed(Error      e,
                time_point now) {
        pending = true;
        last    = e;
        if(failures != std::numeric_limits<std::uint16_t>::max()) { ++failures; }
        next  = now + pause;
        pause = pause >= Longest / 2 ? Longest : pause * 2;
    }

    void succeeded() {
        pending  = false;
        failures = 0;
        pause    = First;
    }

    [[nodiscard]] bool due(time_point now) const { return pending && !(now < next); }
};
}   // namespace Kvasir
