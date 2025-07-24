#pragma once
#include <chrono>
#include <tuple>

namespace Kvasir {
template<typename Bus, typename Clock, typename PowerPin, bool invert, typename... Devs>
struct BusPowerManager {
    using tp = typename Clock::time_point;

    std::tuple<Devs&...> devs;

    enum class State {
        reset,
        poweron,
        poweron_wait,
        idle,
    };

    State st_;
    tp    waitTime_;

    explicit BusPowerManager(Devs&... dev)
      : devs{dev...}
      , st_{State::reset}
      , waitTime_{tp::min()} {}

    template<std::size_t... Is>
    void call_handlerImpl(std::index_sequence<Is...>) {
        ((std::get<Is>(devs).handler()), ...);
    }

    template<std::size_t... Is>
    bool call_needsPowerResetImpl(std::index_sequence<Is...>) {
        return (... || std::get<Is>(devs).needsPowerReset());
    }

    template<std::size_t... Is>
    void call_resetImpl(std::index_sequence<Is...>) {
        return ((std::get<Is>(devs).reset()), ...);
    }

    void call_handler() {
        call_handlerImpl(std::make_index_sequence<std::tuple_size<decltype(devs)>::value>{});
    }

    void call_reset() {
        call_resetImpl(std::make_index_sequence<std::tuple_size<decltype(devs)>::value>{});
    }

    bool needsPowerReset() {
        return call_needsPowerResetImpl(
          std::make_index_sequence<std::tuple_size<decltype(devs)>::value>{});
    }

    constexpr auto clear() {
        if constexpr(invert) {
            return Kvasir::Register::set(PowerPin{});
        } else {
            return Kvasir::Register::clear(PowerPin{});
        }
    }

    constexpr auto set() {
        if constexpr(invert) {
            return Kvasir::Register::clear(PowerPin{});
        } else {
            return Kvasir::Register::set(PowerPin{});
        }
    }

    void handler() {
        auto const currentTime = Clock::now();
        switch(st_) {
        case State::reset:
            {
                st_       = State::poweron;
                waitTime_ = currentTime + 500ms;
                apply(makeOutput(PowerPin{}), clear());
            }
            break;
        case State::poweron:
            {
                if(currentTime > waitTime_) {
                    st_       = State::poweron_wait;
                    waitTime_ = currentTime + 100ms;
                    apply(set());
                }
            }
            break;
        case State::poweron_wait:
            {
                if(currentTime > waitTime_) {
                    st_ = State::idle;
                }
            }
            break;

        case State::idle:
            {
                call_handler();
                if(needsPowerReset()) {
                    call_reset();
                    UC_LOG_W("Bus Power Reset start");
                    st_ = State::reset;
                    // Bus::reset();
                    // TODO bus sw reset?
                }
            }
            break;
        }
    }
};

template<typename Bus,
         typename Clock,
         typename PowerPin,
         bool invert = false,
         typename... Devs>
auto make_BusPowerManager(Devs&... dev) {
    return BusPowerManager<Bus, Clock, PowerPin, invert, decltype(dev)...>{dev...};
}
}   // namespace Kvasir
