#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Kvasir {

/// Watchdog tuning for I2CDeviceBase; override by passing a struct with the same members.
struct I2CDeviceDefaults {
    /// Time after which an in-flight transfer is treated as lost and the device is reset.
    static constexpr auto inFlightTimeout = std::chrono::seconds{2};

    /// Repeat the timeout warning every N consecutive timeouts; 0 logs only the first of
    /// each episode.
    static constexpr std::uint32_t timeoutLogInterval = 0;
};

template<typename I2C,
         typename Clock,
         typename Derived,
         std::size_t ErrorTreshold = 5,
         typename Config           = I2CDeviceDefaults>
struct I2CDeviceBase {
    using tp = typename Clock::time_point;

    constexpr I2CDeviceBase(std::uint8_t address) : i2caddress_{address} {}

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

    void handler() {
        if(inFlight_) {
            if(Clock::now() - inFlightSince_ > Config::inFlightTimeout) {
                inFlight_ = false;
                onInFlightTimeout_();
                auto& self = static_cast<Derived&>(*this);
                self.resetLogic();
                resetErrorCount();
            }
            return;
        }

        auto& self = static_cast<Derived&>(*this);

        if(error_count_ > ErrorTreshold) {
            self.resetLogic();
            resetErrorCount();
        }

        self.idleLogic();
    }

private:
    std::uint8_t const i2caddress_;

    std::atomic<bool>          inFlight_{false};
    tp                         inFlightSince_{};
    std::atomic<std::size_t>   error_count_{};
    std::atomic<std::uint32_t> consecutiveTimeouts_{};

    /// A wedged device would otherwise warn every inFlightTimeout, so only the first
    /// timeout of an episode is logged (plus every timeoutLogInterval-th, if enabled).
    void onInFlightTimeout_() {
        auto const consecutive = consecutiveTimeouts_.fetch_add(1) + 1;
        bool const remind
          = Config::timeoutLogInterval != 0 && consecutive % Config::timeoutLogInterval == 0;
        if(consecutive == 1 || remind) {
            UC_LOG_W("i2c device {:#04x} in-flight watchdog fired (>{}) -- reset, {} in a row",
                     i2caddress_,
                     Config::inFlightTimeout,
                     consecutive);
        }
    }

    template<typename F>
    bool submitQueued_(std::span<std::byte const> sendData,
                       std::span<std::byte>       recvData,
                       F&&                        f) {
        assert(!inFlight_);

        typename I2C::Request req{};
        req.address     = i2caddress_;
        req.sendData    = sendData;
        req.receiveData = recvData;
        req.callback    = [this, func = std::forward<F>(f)](typename I2C::Result r) {
            if(r == I2C::Result::failed || r == I2C::Result::notAcknowledged) {
                incrementErrorCount();
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

        if(!I2C::submit(req)) { return false; }
        inFlight_      = true;
        inFlightSince_ = Clock::now();
        return true;
    }
};

}   // namespace Kvasir
