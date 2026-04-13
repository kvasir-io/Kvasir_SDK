#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Kvasir {

template<typename I2C, typename Clock, typename Derived, std::size_t ErrorTreshold = 5>
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
            if(Clock::now() - inFlightSince_ > kInFlightTimeout) {
                UC_LOG_W("i2c device {:#04x} in-flight watchdog fired (>{}) -- forcing reset",
                         i2caddress_,
                         kInFlightTimeout);
                inFlight_  = false;
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

    static constexpr auto kInFlightTimeout = std::chrono::seconds{2};

    std::atomic<bool>        inFlight_{false};
    tp                       inFlightSince_{};
    std::atomic<std::size_t> error_count_{};

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
