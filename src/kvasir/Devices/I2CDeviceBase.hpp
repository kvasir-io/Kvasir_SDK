#pragma once

#include "kvasir/Devices/SharedBusDevice.hpp"
#include "kvasir/Devices/utils.hpp"
#include "kvasir/Util/StaticFunction.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <span>

namespace Kvasir {
template<typename I2C, typename Clock, typename Derived, std::size_t CallbackSize>
struct I2CDeviceBase : SharedBusDevice<I2C> {
    using tp = typename Clock::time_point;
    using SharedBusDevice<I2C>::acquire;
    using SharedBusDevice<I2C>::release;
    using SharedBusDevice<I2C>::isOwner;
    using SharedBusDevice<I2C>::incementErrorCount;
    using SharedBusDevice<I2C>::resetErrorCount;
    using SharedBusDevice<I2C>::resetHandler;
    using OS = typename I2C::OperationState;

    struct Init {
        explicit Init(tp t) : timeout{t} {}

        tp timeout{};
    };

    struct SendWait {
        Kvasir::StaticFunction<void(), CallbackSize> processor;
    };

    struct ReadWait {
        Kvasir::StaticFunction<void(), CallbackSize> processor;
    };

    std::uint8_t const i2caddress;

    constexpr I2CDeviceBase(std::uint8_t address) : i2caddress{address} {}

    template<typename State,
             typename Data,
             typename F>
    auto startSend(tp           currentTime,
                   State const& fallback,
                   Data const&  data,
                   F&&          onSuccess) {
        using sv = typename Derived::sv;
        if(!acquire()) { return sv{fallback}; }
        I2C::send(currentTime, i2caddress, data);
        return sv{SendWait{std::forward<F>(onSuccess)}};
    }

    template<std::size_t ReadLen,
             typename State,
             typename Data,
             typename F>
    auto startRead(tp           currentTime,
                   State const& fallback,
                   Data const&  data,
                   F&&          onSuccess) {
        using sv = typename Derived::sv;
        if(!acquire()) { return sv{fallback}; }
        I2C::send_receive(currentTime, i2caddress, data, ReadLen);
        return sv{ReadWait{[f = std::forward<F>(onSuccess)] {
            std::array<std::byte, ReadLen> buffer{};
            I2C::getReceivedBytes(buffer);
            f(std::span<std::byte const>{buffer});
        }}};
    }

    void handler() {
        auto&      self        = static_cast<Derived&>(*this);
        auto const currentTime = Clock::now();

        if(self.checkReset()) {
            self.st_.template emplace<Init>(currentTime + Derived::startup_time);
        }

        self.st_ = Kvasir::SM::match(
          self.st_,
          [&](Init const& state) -> typename Derived::sv {
              if(currentTime > state.timeout) { return self.makeIdle(currentTime); }
              return state;
          },
          [&](typename Derived::Idle const& state) ->
          typename Derived::sv { return self.idleHandler(currentTime, state); },
          [&](SendWait const& state) -> typename Derived::sv {
              assert(isOwner());
              switch(I2C::operationState(currentTime)) {
              case OS::ongoing: return state;
              case OS::succeeded:
                  state.processor();
                  release();
                  resetErrorCount();
                  return self.makeSendIdle(currentTime);
              case OS::failed:
                  release();
                  incementErrorCount();
                  return self.makeIdle(currentTime + Derived::fail_retry_time);
              }
              return state;
          },
          [&](ReadWait const& state) -> typename Derived::sv {
              assert(isOwner());
              switch(I2C::operationState(currentTime)) {
              case OS::ongoing: return state;
              case OS::succeeded:
                  {
                      state.processor();
                      release();
                      resetErrorCount();
                      return self.makeReadIdle(currentTime);
                  }
              case OS::failed:
                  release();
                  incementErrorCount();
                  return self.makeReadFailIdle(currentTime);
              }
              return state;
          });
    }
};
}   // namespace Kvasir
