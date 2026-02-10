#pragma once
#include "Utility.hpp"
#include "kvasir/Mpl/Utility.hpp"

namespace Kvasir {
namespace Io {
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

    namespace Detail {
        // make sure we actually got a PinLocation as a parameter
        template<typename TAction, typename TPortPin>
        struct MakeActionIfPinLocation {
            // static_assert(MPL::AlwaysFalse<TAction>::value,"parameter is not a PinLocation");
        };

        template<typename TAction, int Port, int Pin>
        struct MakeActionIfPinLocation<TAction, Register::PinLocation<Port, Pin>> {
            using type = MakeActionT<TAction, Register::PinLocation<Port, Pin>>;
        };

        template<typename TAction, typename TPinLocation>
        using MakeActionIfPinLocationT =
          typename MakeActionIfPinLocation<TAction, TPinLocation>::type;
    }   // namespace Detail

    template<typename TPort,
             typename TPin>
    constexpr Register::PinLocation<TPort::value,
                                    TPin::value>
    makePinLocation(TPort,
                    TPin) {
        return {};
    }
}   // namespace Io

namespace Register {
    template<typename TAction,
             typename T>
    constexpr Io::Detail::MakeActionIfPinLocationT<TAction,
                                                   T>
    action(TAction,
           T) {
        return {};
    }

    template<typename T>
    constexpr Io::Detail::MakeActionIfPinLocationT<
      Io::Action::Input<Io::PullConfiguration::PullNone>,
      T>
    makeInput(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(makeInput(T{}),
                                 makeInput(U{}),
                                 makeInput(Ts{})...)) makeInput(T,
                                                                U,
                                                                Ts...) {
        return {};
    }

    template<typename T>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Output<Io::OutputType::PushPull,
                                                                      Io::OutputSpeed::Low,
                                                                      Io::OutputInit::Low>,
                                                   T>
    makeOutput(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(makeOutput(T{}),
                                 makeOutput(U{}),
                                 makeOutput(Ts{})...)) makeOutput(T,
                                                                  U,
                                                                  Ts...) {
        return {};
    }

    template<typename T>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Output<Io::OutputType::PushPull,
                                                                      Io::OutputSpeed::Low,
                                                                      Io::OutputInit::High>,
                                                   T>
    makeOutputInitHigh(T) {
        return {};
    }

    template<typename TPortPin>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Set,
                                                   TPortPin>
    set(TPortPin) {
        return {};
    }

    template<typename TPortPin>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Clear,
                                                   TPortPin>
    clear(TPortPin) {
        return {};
    }

    template<typename TPortPin>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Toggle,
                                                   TPortPin>
    toggle(TPortPin) {
        return {};
    }

    template<typename TPP1,
             typename TPP2,
             typename... TPortPins>
    constexpr brigand::list<Io::Detail::MakeActionIfPinLocationT<Io::Action::Toggle,
                                                                 TPP1>,
                            Io::Detail::MakeActionIfPinLocationT<Io::Action::Toggle,
                                                                 TPP2>,
                            Io::Detail::MakeActionIfPinLocationT<Io::Action::Toggle,
                                                                 TPortPins>...>
    toggle(TPP1,
           TPP2,
           TPortPins...) {
        return {};
    }

    template<typename TPortPin>
    constexpr Io::Detail::MakeActionIfPinLocationT<Io::Action::Read,
                                                   TPortPin>
    read(TPortPin) {
        return {};
    }

}   // namespace Register
}   // namespace Kvasir
