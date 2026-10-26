#pragma once
#include "Utility.hpp"
#include "kvasir/Mpl/Types.hpp"

namespace Kvasir { namespace Io {
    /// This generic Metafunction which all pin factories resolve to.
    /// It relies on the user to include a chip file which specializes this template
    /// with the proper resulting Register::Action
    template<typename TAction, typename TPinLocation>
    struct MakeAction {
        static_assert(MPL::AlwaysFalse<TAction>::value,
                      "could not find this configuration in the included chip file");
    };

    template<typename TAction, typename TPinLocation>
    using MakeActionT = typename MakeAction<TAction, TPinLocation>::type;

    // a port tag and a pin tag, in that order (makePinLocation(port0, pin25))
    template<int P,
             int N>
    constexpr PinLocation<P,
                          N>
    makePinLocation(HwPort<P>,
                    Pin<N>) {
        return {};
    }

    // The pin factories: a PinLocation becomes the chip's Register::Action (MakeAction), several pins a list of them.
    // They live here, next to PinLocation, and are found by argument-dependent lookup at every unqualified call
    // (`apply(makeOutput(HW::Pin::led{}))`); Register's factories of the same names take fields only.
    template<typename TAction,
             typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<TAction,
                          T>
    action(TAction,
           T) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Input<PullConfiguration::pullNone>,
                          T>
    makeInput(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsPinLocation<T>::value && Detail::IsPinLocation<U>::value
                 && (Detail::IsPinLocation<Ts>::value && ...))
    constexpr decltype(MPL::list(makeInput(T{}),
                                 makeInput(U{}),
                                 makeInput(Ts{})...)) makeInput(T,
                                                                U,
                                                                Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Output<OutputType::pushPull,
                                         OutputSpeed::low,
                                         OutputInit::low>,
                          T>
    makeOutput(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsPinLocation<T>::value && Detail::IsPinLocation<U>::value
                 && (Detail::IsPinLocation<Ts>::value && ...))
    constexpr decltype(MPL::list(makeOutput(T{}),
                                 makeOutput(U{}),
                                 makeOutput(Ts{})...)) makeOutput(T,
                                                                  U,
                                                                  Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Output<OutputType::pushPull,
                                         OutputSpeed::low,
                                         OutputInit::high>,
                          T>
    makeOutputInitHigh(T) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Set,
                          T>
    set(T) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Clear,
                          T>
    clear(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsPinLocation<T>::value && Detail::IsPinLocation<U>::value
                 && (Detail::IsPinLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(clear(T{})),
                                                   decltype(clear(U{})),
                                                   decltype(clear(Ts{}))...>
      clear(T,
            U,
            Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Toggle,
                          T>
    toggle(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsPinLocation<T>::value && Detail::IsPinLocation<U>::value
                 && (Detail::IsPinLocation<Ts>::value && ...))
    constexpr brigand::list<decltype(toggle(T{})),
                            decltype(toggle(U{})),
                            decltype(toggle(Ts{}))...>
      toggle(T,
             U,
             Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsPinLocation<T>::value)
    constexpr MakeActionT<Action::Read,
                          T>
    read(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsPinLocation<T>::value && Detail::IsPinLocation<U>::value
                 && (Detail::IsPinLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(read(T{})),
                                                   decltype(read(U{})),
                                                   decltype(read(Ts{}))...>
      read(T,
           U,
           Ts...) {
        return {};
    }
}}   // namespace Kvasir::Io
