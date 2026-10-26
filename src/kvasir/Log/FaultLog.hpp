#pragma once
// The fault logger of a driver: an `enum class Fault`, a log limiter, the key of each occurrence and, opt-in, a count
// per kind. A site keeps its log line and asks the FaultLog whether it goes out:
//
//     Kvasir::FaultLog<Clock, Fault> faultLog_{};
//     KVASIR_LOG_LIMITED(faultLog_.note(Fault::timeout, now, address), UC_LOG_W, "i2c{} timeout at {:#x}", inst, addr);
//     if(auto const dropped = faultLog_.takeSummary(now); dropped != 0) { UC_LOG_W("+{} faults not logged", dropped); }
//
// note(kind, key...) is `limiter.allow(rateLimitKey(kind, key...), now)` plus the count. The limiter is a
// LogRateLimiter by default: a RateLimiter while logging is compiled in, an empty NoRateLimiter otherwise (no RAM, no
// code), so a decision must only ever decide a log line (a limiter that steers anything else stays a RateLimiter).
//
// Contexts: note() runs in ISRs. The counts are plain saturating `++` on a uint16_t, no read-modify-write atomics (a
// shim call on the Cortex-M0+): a count written from two contexts at once can lose one, the limiter's own tolerance
// (RateLimiter.hpp).
#include "kvasir/Log/RateLimiter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace Kvasir {

namespace Detail {
    struct NoFaultCounts {};
}   // namespace Detail

/// `CountedKinds` > 0: a count per kind (`count(kind)`), kinds 0 .. CountedKinds - 1. 0: no counts, and a FaultLog is
/// exactly the limiter it holds.
template<typename Clock,
         typename Fault,
         RateLimiterConfig Cfg                               = RateLimiterConfig{},
         std::size_t       CountedKinds                      = 0,
         template<typename, RateLimiterConfig> class Limiter = LogRateLimiter>
struct FaultLog {
    using time_point              = typename Clock::time_point;
    static constexpr bool counted = CountedKinds != 0;
    using LimiterT                = Limiter<Clock, Cfg>;

    template<RateLimitKeyPart... Key>
    constexpr RateLimitDecision note(Fault      kind,
                                     time_point now,
                                     Key... key) {
        count_(kind);
        return limiter_.allow(rateLimitKey(kind, key...), now);
    }

    /// `now` from the Clock, as RateLimiter::allow(key).
    template<RateLimitKeyPart... Key>
    RateLimitDecision note(Fault kind,
                           Key... key) {
        count_(kind);
        return limiter_.allow(rateLimitKey(kind, key...));
    }

    /// The occurrences of `kind` since the start, saturating at 65535.
    [[nodiscard]] constexpr std::uint16_t count(Fault kind) const
        requires counted
    {
        return counts_[static_cast<std::size_t>(std::to_underlying(kind))];
    }

    constexpr std::uint32_t takeSummary(time_point now) { return limiter_.takeSummary(now); }

    [[no_unique_address]] LimiterT                                  limiter_{};
    [[no_unique_address]] std::conditional_t<counted,
                                             std::array<std::uint16_t, counted ? CountedKinds : 1>,
                                             Detail::NoFaultCounts> counts_{};

private:
    constexpr void count_(Fault kind) {
        if constexpr(counted) {
            auto& c = counts_[static_cast<std::size_t>(std::to_underlying(kind))];
            if(c != std::numeric_limits<std::uint16_t>::max()) {
                c = static_cast<std::uint16_t>(c + 1U);
            }
        } else {
            static_cast<void>(kind);
        }
    }
};
}   // namespace Kvasir
