#pragma once

#include "kvasir/Util/RateLimiter.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace Kvasir {

/// Tuning for I2CDeviceBase; override by passing a struct with the same members.  Every
/// member is optional: a user struct that lacks one gets the default below.
struct I2CDeviceDefaults {
    /// Time after which an in-flight transfer is treated as lost and the device is reset.
    static constexpr auto inFlightTimeout = std::chrono::seconds{2};

    /// Repeat the timeout warning every N consecutive timeouts; 0 logs only the first of
    /// each episode.
    static constexpr std::uint32_t timeoutLogInterval = 0;

    /// Consecutive NAKs after which the device is treated as absent and parked; 0 disables.
    static constexpr std::uint8_t absentAfterNaks = 3;

    /// A parked device is probed with a single request per interval; the interval doubles
    /// after each failed probe up to probeIntervalMax.
    static constexpr auto probeInterval    = std::chrono::seconds{1};
    static constexpr auto probeIntervalMax = std::chrono::seconds{30};
};

/// CRTP base for a device on a queued I2C bus (Kvasir::I2C::I2CBehaviorQueued).
///
/// Derived provides resetLogic() and idleLogic(); handler() must run once per main-loop
/// iteration.  The base owns:
///   * the in-flight watchdog (a transfer whose callback never comes resets the device),
///   * the error threshold (ErrorTreshold failures -> resetLogic()),
///   * absent-device handling: after absentAfterNaks consecutive NAKs the device is
///     parked -- idleLogic() is not called and submit*() returns false -- and probed with
///     one request per probeInterval (1 s, 2 s, ... probeIntervalMax).  The driver's own
///     init request, issued from resetLogic()/idleLogic(), is the probe; a NAK keeps the
///     device parked, an ACK unparks it at once.  present() tells the state.
template<typename I2C,
         typename Clock,
         typename Derived,
         std::size_t ErrorTreshold = 5,
         typename Config           = I2CDeviceDefaults>
struct I2CDeviceBase {
    using tp = typename Clock::time_point;

    static constexpr std::chrono::milliseconds InFlightTimeout = [] {
        if constexpr(requires { Config::inFlightTimeout; }) {
            return Config::inFlightTimeout;
        } else {
            return I2CDeviceDefaults::inFlightTimeout;
        }
    }();

    static constexpr std::uint32_t TimeoutLogInterval = [] {
        if constexpr(requires { Config::timeoutLogInterval; }) {
            return Config::timeoutLogInterval;
        } else {
            return I2CDeviceDefaults::timeoutLogInterval;
        }
    }();

    static constexpr std::uint8_t AbsentAfterNaks = [] {
        if constexpr(requires { Config::absentAfterNaks; }) {
            return Config::absentAfterNaks;
        } else {
            return I2CDeviceDefaults::absentAfterNaks;
        }
    }();

    static constexpr std::chrono::milliseconds ProbeInterval = [] {
        if constexpr(requires { Config::probeInterval; }) {
            return Config::probeInterval;
        } else {
            return I2CDeviceDefaults::probeInterval;
        }
    }();

    static constexpr std::chrono::milliseconds ProbeIntervalMax = [] {
        if constexpr(requires { Config::probeIntervalMax; }) {
            return Config::probeIntervalMax;
        } else {
            return I2CDeviceDefaults::probeIntervalMax;
        }
    }();

    constexpr I2CDeviceBase(std::uint8_t address) : i2caddress_{address} {}

    /// All submit* return false when the request could not be queued: bus queue full, or
    /// the device is parked as absent and no probe is due.
    template<typename F>
    bool submitSend(std::span<std::byte const> sendData,
                    F&&                        f) {
        return submitQueued_(sendData, {}, std::forward<F>(f));
    }

    template<typename F>
    bool submitRead(std::span<std::byte const> sendData,
                    std::span<std::byte>       recvData,
                    F&&                        f) {
        return submitQueued_(sendData, recvData, std::forward<F>(f));
    }

    template<typename F>
    bool submitReceive(std::span<std::byte> recvData,
                       F&&                  f) {
        return submitQueued_({}, recvData, std::forward<F>(f));
    }

    void incrementErrorCount() { ++error_count_; }

    void resetErrorCount() { error_count_ = 0; }

    /// False while the device is parked because it stopped acknowledging.
    bool present() const { return !absent_.load(std::memory_order_relaxed); }

    std::uint8_t consecutiveNaks() const {
        return consecutiveNaks_.load(std::memory_order_relaxed);
    }

    void handler() {
        if(inFlight_) {
            if(Clock::now() - inFlightSince_ > InFlightTimeout) {
                inFlight_ = false;
                onInFlightTimeout_();
                auto& self = static_cast<Derived&>(*this);
                self.resetLogic();
                resetErrorCount();
            }
            return;
        }

        auto&      self = static_cast<Derived&>(*this);
        auto const now  = Clock::now();

        if(returned_.exchange(false)) {
            KVASIR_LOG_LIMITED(presenceLog_.allow(PresentAgain, now),
                               UC_LOG_I,
                               "i2c device {:#04x} present again after {} probe(s)",
                               i2caddress_,
                               probes_);
            resetErrorCount();
        }

        if(AbsentAfterNaks != 0 && !absent_.load(std::memory_order_relaxed)
           && consecutiveNaks_.load(std::memory_order_relaxed) >= AbsentAfterNaks)
        {
            absent_        = true;
            probeArmed_    = false;
            probes_        = 0;
            probeInterval_ = ProbeInterval;
            nextProbe_     = now + probeInterval_;
            KVASIR_LOG_LIMITED(presenceLog_.allow(NotResponding, now),
                               UC_LOG_W,
                               "i2c device {:#04x} not responding ({} NAKs in a row) -- "
                               "probing every {} .. {}",
                               i2caddress_,
                               consecutiveNaks_.load(std::memory_order_relaxed),
                               ProbeInterval,
                               ProbeIntervalMax);
            self.resetLogic();
            resetErrorCount();
            return;
        }

        if(absent_.load(std::memory_order_relaxed) && !probeArmed_.load(std::memory_order_relaxed))
        {
            if(now < nextProbe_) { return; }   // parked: no idleLogic(), no bus traffic
            probeArmed_ = true;
            ++probes_;
            nextProbe_     = now + probeInterval_;
            probeInterval_ = std::min(probeInterval_ * 2, ProbeIntervalMax);
            self.resetLogic();   // the driver's init request becomes the probe
        }

        if(error_count_ > ErrorTreshold) {
            self.resetLogic();
            resetErrorCount();
        }

        self.idleLogic();
    }

private:
    static constexpr std::uint32_t NotResponding = rateLimitKey(1);
    static constexpr std::uint32_t PresentAgain  = rateLimitKey(2);

    std::uint8_t const i2caddress_;

    std::atomic<bool>          inFlight_{false};
    tp                         inFlightSince_{};
    std::atomic<std::size_t>   error_count_{};
    std::atomic<std::uint32_t> consecutiveTimeouts_{};

    std::atomic<std::uint8_t> consecutiveNaks_{};
    std::atomic<bool>         absent_{false};
    std::atomic<bool>         probeArmed_{false};   // one request may pass the gate
    std::atomic<bool>         returned_{false};     // ACKed while absent; report from handler()
    tp                        nextProbe_{};
    std::chrono::milliseconds probeInterval_{ProbeInterval};
    std::uint16_t             probes_{};
    RateLimiter<Clock>        presenceLog_{};   // a flapping device must not flood either

    /// A wedged device would otherwise warn every inFlightTimeout, so only the first
    /// timeout of an episode is logged (plus every timeoutLogInterval-th, if enabled).
    void onInFlightTimeout_() {
        auto const consecutive = consecutiveTimeouts_.fetch_add(1) + 1;
        bool const remind      = [&] {
            if constexpr(TimeoutLogInterval != 0) {
                return consecutive % TimeoutLogInterval == 0;
            } else {
                return false;
            }
        }();
        if(consecutive == 1 || remind) {
            UC_LOG_W("i2c device {:#04x} in-flight watchdog fired (>{}) -- reset, {} in a row",
                     i2caddress_,
                     InFlightTimeout,
                     consecutive);
        }
    }

    template<typename F>
    bool submitQueued_(std::span<std::byte const> sendData,
                       std::span<std::byte>       recvData,
                       F&&                        f) {
        assert(!inFlight_);

        // Parked: only the one probe per interval that handler() armed may pass.
        if(absent_.load(std::memory_order_relaxed) && !probeArmed_.exchange(false)) {
            return false;
        }

        typename I2C::Request req{};
        req.address     = i2caddress_;
        req.sendData    = sendData;
        req.receiveData = recvData;
        req.callback    = [this, func = std::forward<F>(f)](typename I2C::Result r) {
            if(r == I2C::Result::notAcknowledged) {
                // a NAK is the device not answering; a bus fault (failed) says nothing
                // about its presence and leaves the streak alone
                auto const n = consecutiveNaks_.load(std::memory_order_relaxed);
                if(n != std::numeric_limits<std::uint8_t>::max()) {
                    consecutiveNaks_.store(static_cast<std::uint8_t>(n + 1),
                                           std::memory_order_relaxed);
                }
                incrementErrorCount();
            } else if(r == I2C::Result::failed) {
                incrementErrorCount();
            } else {
                consecutiveNaks_.store(0, std::memory_order_relaxed);
                if(absent_.exchange(false)) { returned_ = true; }
            }
            if(auto const timeouts = consecutiveTimeouts_.load(); timeouts != 0) {
                UC_LOG_I("i2c device {:#04x} answering again after {} in-flight timeout(s)",
                         i2caddress_,
                         timeouts);
                consecutiveTimeouts_ = 0;
            }
            inFlight_ = false;
            func(r);
        };

        // Armed BEFORE the submit: the bus driver starts the request inside submit() and a
        // one-byte write completes in the ISR ~25 us later, so the callback can run -- and
        // clear inFlight_ -- before submit() has returned to this line. Setting the flag
        // afterwards then left it set for good and the watchdog fired 2 s later on a
        // request that had long succeeded (BH1750 power-on, i2c_testing, 2026-09-04).
        inFlightSince_ = Clock::now();
        inFlight_      = true;
        if(!I2C::submit(req)) {
            inFlight_ = false;
            return false;
        }
        return true;
    }
};

}   // namespace Kvasir
