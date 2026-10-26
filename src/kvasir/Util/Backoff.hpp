#pragma once
// A pause before the next attempt that doubles after every failure up to a ceiling and starts over after a success,
// and the doubling step on its own. Retry.hpp (a store's pending write) is a Backoff with the pending value's state; a
// component that only needs the schedule uses Backoff:
//
//     Kvasir::Backoff<Clock> backoff_{};
//     if(ok) { backoff_.succeeded(); } else { backoff_.failed(Clock::now()); }
//     if(backoff_.due(Clock::now())) { /* try again */ }
//
// A clock that jumps (a loop that was blocked for minutes) makes it due once, not once per missed pause.
#include <chrono>
#include <cstdint>
#include <limits>

namespace Kvasir {

/// The defaults of a Backoff and a Retry: 1 s, doubling, at most 10 min; a pending Retry reads as Link::absent
/// (linkOf, Retry.hpp) from the third failure in a row on.
struct RetryDefaults {
    static constexpr std::chrono::seconds first{1};
    static constexpr std::chrono::minutes longest{10};
    static constexpr std::uint16_t        absentAfter = 3;
};

/// The next pause of a doubling backoff: twice `d`, never past `longest`, without overflow for any duration type (an
/// unsigned 32-bit Millis32 included).
template<typename D>
[[nodiscard]] constexpr D doubledUpTo(D d,
                                      D longest) {
    return d >= longest / 2 ? longest : d * 2;
}

/// Cfg: `first` and `longest` as chrono durations (RetryDefaults).
template<typename Clock, typename Cfg = RetryDefaults>
struct Backoff {
    using duration   = typename Clock::duration;
    using time_point = typename Clock::time_point;

    static constexpr duration first   = std::chrono::duration_cast<duration>(Cfg::first);
    static constexpr duration longest = std::chrono::duration_cast<duration>(Cfg::longest);
    static_assert(first > duration::zero() && first <= longest,
                  "a backoff pause must be positive and the first no longer than the longest");

    // failures last: a Retry (Retry.hpp) puts its pending/last into the tail padding behind it
    duration      pause{first};   ///< the wait after the next failure
    time_point    next{};         ///< after a failure: the earliest next attempt
    std::uint16_t failures{};     ///< since the last success, saturating

    void failed(time_point now) {
        if(failures != std::numeric_limits<std::uint16_t>::max()) { ++failures; }
        next  = now + pause;
        pause = doubledUpTo(pause, longest);
    }

    void succeeded() {
        failures = 0;
        pause    = first;
    }

    /// A failure is waiting and its pause is over.
    [[nodiscard]] bool due(time_point now) const { return failures != 0 && !(now < next); }
};
}   // namespace Kvasir
