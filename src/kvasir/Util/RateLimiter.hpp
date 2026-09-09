#pragma once

// Rate limiting for fault reporting.
//
// A fault caused by the environment -- a dead I2C bus, a flaky USB cable, a noisy
// UART line -- repeats on every transaction.  Logging it unconditionally floods the
// log and, since most of these paths run in an ISR, spends ISR time serializing the
// same line over and over.  The limiters here bound that:
//
//   RateLimiter<Clock>   time based.  A storm of faults is reported with exponential
//                        backoff (1 s, 2 s, 4 s ... 32 s between lines); within a storm
//                        each distinct fault (key) is shown once when it first appears.
//                        A burst budget caps the total rate.
//   CountLimiter         occurrence based, for code without a clock.  Reports the 1st
//                        occurrence, then with doubling gaps, then every Nth.
//
// Both hand the number of dropped occurrences back with the next allowed one, so
// nothing disappears silently.  KVASIR_LOG_LIMITED wraps the "log line plus dropped
// count" pattern: the count is appended to the same line as " (+N not logged)".
//
// Concurrency: a limiter is plain data.  Concurrent use from an ISR and the main loop
// can at worst miscount or misjudge a single occurrence, never break anything; where
// exact behaviour matters, bracket allow()/takeSummary() with the ISR disabled.

#include <algorithm>
#include <array>
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

// Log argument that renders as " (+N not logged)" -- or as nothing when N is 0.
struct Suppressed {
    std::uint32_t n{};
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
    std::uint32_t windowMs   = 1000;   // first repeat window and burst budget period
    std::uint8_t  burst      = 4;      // reports allowed per window, over all keys
    std::uint8_t  maxBackoff = 5;      // repeat window doubles this many times: 1 s .. 32 s
};

// Model: a "storm" starts with the first fault after a quiet gap and lasts until the
// faults stop for QuietGap.  Within a storm:
//   * anchor reports happen at 0, 1, 3, 7, 15, 31, 63 ... s (the window doubles up to
//     maxBackoff) regardless of which key triggers them;
//   * a key that has not been seen in this storm is reported once, immediately, without
//     touching the anchor schedule -- so a second failing device or a different abort
//     cause is visible, but cannot restart the backoff.  The last SeenKeys distinct keys
//     are remembered exactly; a storm with more kinds than that re-shows the oldest;
//   * everything else is counted and handed back with the next report.
// The burst budget bounds the total (anchor + detail) rate per window.
template<typename Clock, RateLimiterConfig Cfg = RateLimiterConfig{}>
struct RateLimiter {
    using tp = typename Clock::time_point;

    static constexpr auto        Window   = std::chrono::milliseconds{Cfg.windowMs};
    static constexpr auto        QuietGap = Window * (2U << Cfg.maxBackoff);   // 2 x longest window
    static constexpr std::size_t SeenKeys = 4;

    constexpr auto repeatWindow() const { return Window * (1U << backoff_); }

    // Report this occurrence?  `now` is taken from Clock if not given.
    constexpr RateLimitDecision allow(std::uint32_t key,
                                      tp            now) {
        if(now - budgetStart_ >= Window) {
            budgetStart_ = now;
            budget_      = Cfg.burst;
        }

        // seenCount_ == 0 covers the very first fault (and reset()): a clock that starts
        // near zero must not make it look like a repeat of a report at the epoch.
        bool const fresh = seenCount_ == 0 || now - lastOccurrence_ >= QuietGap;
        lastOccurrence_  = now;
        if(fresh) {
            backoff_   = 0;
            seenCount_ = 0;
            seenNext_  = 0;
        }

        bool const due    = fresh || now - lastReport_ >= repeatWindow();
        bool const detail = !seen(key);
        if(budget_ == 0 || !(due || detail)) {
            ++suppressed_;
            return {false, suppressed_};
        }

        --budget_;
        if(detail) { remember(key); }
        if(due) {
            if(!fresh) { backoff_ = std::min<std::uint8_t>(backoff_ + 1, Cfg.maxBackoff); }
            lastReport_ = now;
        }
        return {true, std::exchange(suppressed_, 0U)};
    }

    RateLimitDecision allow(std::uint32_t key = 0) { return allow(key, Clock::now()); }

    // For pollers: the dropped count once the faults have paused for a repeat window,
    // so a storm that ended is reported without waiting for the next fault.  0 while
    // it is still running.  Clears the count.
    constexpr std::uint32_t takeSummary(tp now) {
        if(suppressed_ == 0 || now - lastOccurrence_ < repeatWindow()) { return 0; }
        return std::exchange(suppressed_, 0U);
    }

    constexpr std::uint32_t suppressed() const { return suppressed_; }

    constexpr void reset() { *this = RateLimiter{}; }

private:
    constexpr bool seen(std::uint32_t key) const {
        for(std::size_t i = 0; i < seenCount_; ++i) {
            if(seen_[i] == key) { return true; }
        }
        return false;
    }

    constexpr void remember(std::uint32_t key) {
        seen_[seenNext_] = key;
        seenNext_        = static_cast<std::uint8_t>((seenNext_ + 1) % SeenKeys);
        seenCount_       = std::min<std::uint8_t>(seenCount_ + 1, SeenKeys);
    }

    std::array<std::uint32_t, SeenKeys> seen_{};   // keys reported in this storm
    std::uint8_t                        seenCount_{};
    std::uint8_t                        seenNext_{};   // slot the next new key evicts
    std::uint32_t                       suppressed_{};
    std::uint8_t                        backoff_{};
    std::uint8_t                        budget_{Cfg.burst};
    tp lastReport_{};       // last anchor report; drives the backoff schedule
    tp lastOccurrence_{};   // last call, allowed or not; drives storm boundaries
    tp budgetStart_{};
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

#if __has_include("remote_fmt/remote_fmt.hpp")
    #include "remote_fmt/remote_fmt.hpp"

// Nothing for 0, " (+N not logged)" otherwise -- so a normal line stays as it was and
// only a line that stands for dropped ones carries the count.
template<>
struct remote_fmt::formatter<Kvasir::Suppressed> {
    template<typename Printer>
    constexpr auto format(Kvasir::Suppressed const& s,
                          Printer&                  printer) const {
        if(s.n == 0) { return format_to(printer, SC_LIFT("")); }
        return format_to(printer, SC_LIFT(" (+{} not logged)"), s.n);
    }
};
#endif

#if __has_include("uc_log/uc_log.hpp")
    #include "uc_log/uc_log.hpp"

    // Logs through LOG (UC_LOG_W, UC_LOG_E, ...) only when `decision` allows it, with
    // the occurrences dropped since the previous line appended to the same message:
    //
    //   KVASIR_LOG_LIMITED(faultLog_.allow(key), UC_LOG_W, "i2c{} abort {}", inst, src);
    //   -> "i2c0 abort ..."                    normally
    //   -> "i2c0 abort ... (+199 not logged)"  when 199 were suppressed before it
    //
    // `fmt` must be a string literal (it is concatenated with the count placeholder).
    #define KVASIR_LOG_LIMITED(decision, LOG, fmt, ...)                \
        do {                                                           \
            if(auto const kvasir_limited_ = (decision)) {              \
                LOG(fmt "{}" __VA_OPT__(, ) __VA_ARGS__,               \
                    ::Kvasir::Suppressed{kvasir_limited_.suppressed}); \
            }                                                          \
        } while(false)
#endif
