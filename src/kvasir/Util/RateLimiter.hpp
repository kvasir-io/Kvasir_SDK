#pragma once

// Rate limiting for fault reporting.
//
// A fault caused by the environment -- a dead I2C bus, a flaky USB cable, a noisy
// UART line -- repeats on every transaction.  Logging it unconditionally floods the
// log and, since most of these paths run in an ISR, spends ISR time serializing the
// same line over and over.  The limiters here bound that:
//
//   RateLimiter<Clock>   time based.  An identical fault (same key) is reported once,
//                        then backs off exponentially while it keeps repeating; a new
//                        key is reported at once.  A global burst budget caps the total
//                        rate so alternating keys cannot defeat the backoff.
//   CountLimiter         occurrence based, for code without a clock.  Reports the 1st
//                        occurrence, then with doubling gaps, then every Nth.
//
// Both hand the number of dropped occurrences back with the next allowed one, so
// nothing disappears silently.  KVASIR_LOG_LIMITED wraps the common "log line plus
// dropped count" pattern.
//
// Concurrency: a limiter is plain data.  Concurrent use from an ISR and the main loop
// can at worst miscount or misjudge a single occurrence, never break anything; where
// exact behaviour matters, bracket allow()/takeSummary() with the ISR disabled.

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Kvasir {

// Result of a limiter query.  Converts to bool: "report it".  `suppressed` is the
// number of occurrences dropped since the previous reported one.
struct RateLimitDecision {
    bool          allowed{};
    std::uint32_t suppressed{};

    constexpr explicit operator bool() const { return allowed; }
};

template<typename T>
concept RateLimitKeyPart = std::integral<T> || std::is_enum_v<T>;

// Folds integers, bools and enums into a key identifying one kind of fault, e.g.
// rateLimitKey(Fault::abort, address, abortSource).  A collision merely merges two
// faults for limiting purposes.
constexpr std::uint32_t rateLimitKey(RateLimitKeyPart auto... parts) {
    std::uint32_t h = 0x811C'9DC5U;
    ((h = (h ^ static_cast<std::uint32_t>(parts)) * 0x9E37'79B9U), ...);
    return h;
}

struct RateLimiterConfig {
    std::uint32_t windowMs   = 1000;   // base repeat window and burst budget period
    std::uint8_t  burst      = 4;      // reports allowed per window, over all keys
    std::uint8_t  maxBackoff = 3;      // repeat window doubles this many times: 1s..8s
};

template<typename Clock, RateLimiterConfig Cfg = RateLimiterConfig{}>
struct RateLimiter {
    using tp = typename Clock::time_point;

    static constexpr auto Window = std::chrono::milliseconds{Cfg.windowMs};

    constexpr auto repeatWindow() const { return Window * (1U << backoff_); }

    // Report this occurrence?  `now` is taken from Clock if not given.
    constexpr RateLimitDecision allow(std::uint32_t key,
                                      tp            now) {
        if(now - budgetStart_ >= Window) {
            budgetStart_ = now;
            budget_      = Cfg.burst;
        }

        bool const isRepeat = key == key_;
        if((isRepeat && now - lastReport_ < repeatWindow()) || budget_ == 0) {
            ++suppressed_;
            return {false, suppressed_};
        }

        if(budget_ != 0) { --budget_; }
        backoff_
          = isRepeat ? std::min<std::uint8_t>(backoff_ + 1, Cfg.maxBackoff) : std::uint8_t{0};
        key_        = key;
        lastReport_ = now;
        return {true, std::exchange(suppressed_, 0U)};
    }

    RateLimitDecision allow(std::uint32_t key = 0) { return allow(key, Clock::now()); }

    // For pollers: the dropped count once the fault has stopped repeating, so a storm
    // that ended is reported without waiting for the next fault.  0 while it is still
    // running.  Clears the count.
    constexpr std::uint32_t takeSummary(tp now) {
        if(suppressed_ == 0 || now - lastReport_ < repeatWindow()) { return 0; }
        return std::exchange(suppressed_, 0U);
    }

    constexpr std::uint32_t suppressed() const { return suppressed_; }

    constexpr void reset() { *this = RateLimiter{}; }

private:
    std::uint32_t key_{};
    std::uint32_t suppressed_{};
    std::uint8_t  backoff_{};
    std::uint8_t  budget_{Cfg.burst};
    tp            lastReport_{};
    tp            budgetStart_{};
};

struct CountLimiterConfig {
    std::uint32_t maxGap = 1024;   // once the gap between reports reaches this it stays
};

template<CountLimiterConfig Cfg = CountLimiterConfig{}>
struct CountLimiter {
    // Report this occurrence?  Reports the 1st, then after 1, 2, 4, ... maxGap dropped.
    constexpr RateLimitDecision allow() {
        if(dropped_ < gap_) {
            ++dropped_;
            return {false, dropped_};
        }
        gap_ = gap_ == 0 ? 1 : std::min(gap_ * 2, Cfg.maxGap);
        return {true, std::exchange(dropped_, 0U)};
    }

    constexpr std::uint32_t suppressed() const { return dropped_; }

    constexpr void reset() { *this = CountLimiter{}; }

private:
    std::uint32_t dropped_{};
    std::uint32_t gap_{0};   // first occurrence is always reported
};

}   // namespace Kvasir

#if __has_include("uc_log/uc_log.hpp")
    #include "uc_log/uc_log.hpp"

    // Logs through LOG (UC_LOG_W, UC_LOG_E, ...) only when `decision` allows it, and
    // then also reports the occurrences dropped since the previous line:
    //
    //   KVASIR_LOG_LIMITED(faultLog_.allow(key), UC_LOG_W, "i2c{} abort {}", inst, src);
    //
    // The line numbers of both messages are the call site's.
    #define KVASIR_LOG_LIMITED(decision, LOG, ...)                                            \
        do {                                                                                  \
            if(auto const kvasir_limited_ = (decision)) {                                     \
                LOG(__VA_ARGS__);                                                             \
                if(kvasir_limited_.suppressed != 0) {                                         \
                    LOG("... {} earlier occurrences not logged", kvasir_limited_.suppressed); \
                }                                                                             \
            }                                                                                 \
        } while(false)
#endif
