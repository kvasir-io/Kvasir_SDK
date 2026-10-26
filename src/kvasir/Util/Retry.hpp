#pragma once
// A write that has not reached its target yet and is tried again by itself, with a pause that doubles after every
// failure up to a ceiling, and starts over after a success. A store keeps one and asks `due(now)` from its
// Hook::MainLoop function:
//
//     static inline Kvasir::Retry<Clock, Error> retry_{};
//     if(auto const r = attempt(); r) { retry_.succeeded(); } else { retry_.failed(r.error(), Clock::now()); }
//     ...
//     static void turn() { if(retry_.due(Clock::now())) { (void)attempt(); ... } }
//
// A clock that jumps (a loop that was blocked for minutes) makes it due once, not once per missed pause. The schedule
// is Backoff.hpp's: a Retry is a Backoff (failures, pause, next) plus the pending value's state (pending, last).
//
// A store's link (Util/Link.hpp) is `Kvasir::linkOf(retry_)`: answering while nothing is pending, starting while a
// write waits for its first retries, absent from Cfg::absentAfter failures in a row on (RetryDefaults: 3). It goes
// back to answering by itself with the first write that lands.
#include "kvasir/Util/Backoff.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Link.hpp"

#include <cstdint>

namespace Kvasir {

template<typename Clock, typename Error, typename Cfg = RetryDefaults>
struct Retry : Backoff<Clock, Cfg> {
    using time_point = typename Backoff<Clock, Cfg>::time_point;

    bool  pending{};   ///< a value that has not reached its target (exactly while failures != 0)
    Error last{};      ///< the error of the last failed attempt

    void failed(Error      e,
                time_point now) {
        pending = true;
        last    = e;
        Backoff<Clock, Cfg>::failed(now);
    }

    void succeeded() {
        pending = false;
        Backoff<Clock, Cfg>::succeeded();
    }
};

/// What a store with this pending write is, as a Link (Util/Link.hpp).
template<typename Clock,
         typename Error,
         typename Cfg>
[[nodiscard]] constexpr Link linkOf(Retry<Clock,
                                          Error,
                                          Cfg> const& r) {
    constexpr std::uint16_t absentAfter
      = KVASIR_CONFIG_OR(Cfg, absentAfter, RetryDefaults::absentAfter);
    static_assert(absentAfter != 0, "a store is absent after at least one failure");
    if(!r.pending) { return Link::answering; }
    return r.failures >= absentAfter ? Link::absent : Link::starting;
}
}   // namespace Kvasir
