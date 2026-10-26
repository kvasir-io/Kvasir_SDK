#pragma once
// What every RAM-copy store in flash shares, written once: a value that did not reach the flash stays pending and is
// written again by itself (Kvasir::Retry, a pause doubling up to 10 min), its link (Util/Link.hpp), and the
// Hook::MainLoop function turn() that does the retry - the store listed in the Startup list. The stores: SAM
// EepromEmulator / FlashJournal (chip_atsam_common NVMCTRL.hpp), Kvasir::Flash::CellEeprom (CellStore.hpp).
//
//     struct MyStore : Kvasir::Flash::RetryingStore<MyStore, Clock, Error> {
//         static bool writeValue() { return settle(attempt()); }   // settle: the retry and the log lines
//         static bool retryNow() { return writeValue(); }           // what turn() runs when the retry is due
//         // optional, the store's own log text:
//         static void logFailed(Retry const& r, Error e, Clock::duration untilNext);
//         static void logRecovered(Retry const& r);
//     };
//
// A Clock is required: every failed write is retried until it lands (no store waits for a person). writeValue()'s
// meaning is the store's own: SAM stores answer "the flash holds the value", CellEeprom "it wrote".
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Util/Link.hpp"
#include "kvasir/Util/Result.hpp"
#include "kvasir/Util/Retry.hpp"

#include <chrono>
#include <concepts>
#include <expected>

namespace Kvasir::Flash {

/// The surface every RAM-copy store offers.
template<typename S>
concept CachedStore = requires {
    { S::writeValue() } -> std::same_as<bool>;
    { S::pending() } -> std::same_as<bool>;
    { S::link() } -> std::same_as<Kvasir::Link>;
    S::turn();
    typename S::Extends;
};

template<typename Derived, typename Clock, typename Error>
struct RetryingStore {
    using Retry = Kvasir::Retry<Clock, Error>;

    /// A value that has not reached the flash yet.
    [[nodiscard]] static bool pending() { return retry_.pending; }

    /// Answering while nothing is pending, starting while a write waits for its retries, absent from the third
    /// failure in a row (Kvasir::linkOf); the first write that lands brings it back.
    [[nodiscard]] static Kvasir::Link link() { return Kvasir::linkOf(retry_); }

    [[nodiscard]] static Retry const& retry() { return retry_; }

    static void turn() {
        if(retry_.due(Clock::now())) { static_cast<void>(Derived::retryNow()); }
    }

    using Extends
      = brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &RetryingStore::turn>>;

protected:
    /// After an attempt (anything with has_value() and error()): the retry, and the store's log lines; true on
    /// success.
    template<typename R>
    static bool settle(R const& r) {
        if(r.has_value()) {
            if(retry_.pending) {
                if constexpr(requires { Derived::logRecovered(retry_); }) {
                    Derived::logRecovered(retry_);
                }
            }
            retry_.succeeded();
            return true;
        }
        auto const now = Clock::now();
        retry_.failed(r.error(), now);
        if constexpr(requires { Derived::logFailed(retry_, r.error(), retry_.next - now); }) {
            Derived::logFailed(retry_, r.error(), retry_.next - now);
        }
        return false;
    }

    /// Nothing pending any more (a test's "reboot").
    static void resetRetry() { retry_ = {}; }

    static inline Retry retry_{};
};

/// A RAM copy of T over a store with readFlashValue() and internalWrite() -> Kvasir::Result<void, Error> (an Error
/// with a `verify`): value() reads the flash once, writeValue() writes the RAM copy when it differs and reads it back;
/// true when the flash holds it.
template<typename Derived, typename T, typename Clock, typename Error>
struct CachedValue : RetryingStore<Derived, Clock, Error> {
    static T& value() {
        if(!valueRead) {
            ramCopy   = Derived::readFlashValue();
            valueRead = true;
        }
        return ramCopy;
    }

    /// The RAM copy into the flash if it differs; true when the flash holds it. A failure stays pending and is
    /// written again by turn(); retry().last says why. (A bool, not a Result: libstdc++'s std::expected is
    /// [[nodiscard]], and a caller may leave the store to it.)
    static bool writeValue() { return CachedValue::settle(attempt()); }

    static bool retryNow() { return writeValue(); }

    static Kvasir::Result<void,
                          Error>
    attempt() {
        if(ramCopy == Derived::readFlashValue()) { return {}; }
        if(auto const r = Derived::internalWrite(); !r) { return r; }
        if(ramCopy != Derived::readFlashValue()) { return std::unexpected{Error::verify}; }
        return {};
    }

    static inline T    ramCopy{};
    static inline bool valueRead{false};
};
}   // namespace Kvasir::Flash
