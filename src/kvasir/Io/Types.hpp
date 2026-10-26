#pragma once

#include <type_traits>

namespace Kvasir { namespace Io {
    enum class PullConfiguration { pullNone, pullUp, pullDown };
    enum class OutputType { pushPull, openDrain };
    enum class OutputInit { low, high };
    enum class OutputSpeed { low, medium, high, veryHigh };

    namespace Action {
        template<PullConfiguration PC>
        struct Input {};

        template<OutputType OT, OutputSpeed OS, OutputInit OI>
        struct Output {};

        struct Set {};

        struct Clear {};

        struct Toggle {};

        struct Read {};

        struct Analog {};

        template<int               I,
                 OutputType        OT = OutputType::pushPull,
                 OutputSpeed       OS = OutputSpeed::low,
                 OutputInit        OI = OutputInit::low,
                 PullConfiguration PC = PullConfiguration::pullNone>
        struct PinFunction {
            static constexpr int value = I;
        };

        static constexpr Input<PullConfiguration::pullNone> inputPN{};
        static constexpr Input<PullConfiguration::pullUp>   inputPU{};
        static constexpr Input<PullConfiguration::pullDown> inputPD{};
    }   // namespace Action

    // A port and a pin number as types (the constants port0.., pin0.. of Io.hpp): MPL::IntegralConstants with their
    // int conversion deleted, so neither passes as a runtime int, and `type` is the tag itself, not the base.
    template<int I>
    struct HwPort : std::integral_constant<int, I> {
        using type = HwPort<I>;

        operator int() const = delete;
    };

    template<int I>
    struct Pin : std::integral_constant<int, I> {
        using type = Pin<I>;

        operator int() const = delete;
    };

    // Io Pin which is not used
    template<typename = void>
    struct NotUsed {};

    // A pin as a type; the pin factories (PinFactories.hpp) are found through it by argument-dependent lookup.
    template<int Port, int Pin>
    struct PinLocation {
        using type = PinLocation<Port, Pin>;

        static constexpr int port = Port;
        static constexpr int pin  = Pin;
    };

    /// The GPIO number a PinLocation names, as a value: for a log line or a static_assert
    /// about a board's pin choice (`pinNumber(HW::Pin::led{})`).
    template<int Port,
             int Pin>
    constexpr int pinNumber(PinLocation<Port,
                                        Pin>) {
        return Pin;
    }
}}   // namespace Kvasir::Io
