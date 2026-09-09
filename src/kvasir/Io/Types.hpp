#pragma once
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/StartUp/Resources.hpp"

namespace Kvasir {
namespace Io {
    enum class PullConfiguration { PullNone, PullUp, PullDown };
    enum class OutputType { PushPull, OpenDrain };
    enum class OutputInit { Low, High };
    enum class OutputSpeed { Low, Medium, High, VeryHigh };

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
                 OutputType        OT = OutputType::PushPull,
                 OutputSpeed       OS = OutputSpeed::Low,
                 OutputInit        OI = OutputInit::Low,
                 PullConfiguration PC = PullConfiguration::PullNone>
        struct PinFunction {
            static constexpr int value = I;
        };

        static constexpr Input<PullConfiguration::PullNone>                              input{};
        static constexpr Input<PullConfiguration::PullNone>                              inputPN{};
        static constexpr Input<PullConfiguration::PullUp>                                inputPU{};
        static constexpr Input<PullConfiguration::PullDown>                              inputPD{};
        static constexpr Output<OutputType::PushPull, OutputSpeed::Low, OutputInit::Low> output{};
        static constexpr Output<OutputType::PushPull, OutputSpeed::Low, OutputInit::Low> outputPP{};
        static constexpr Output<OutputType::OpenDrain, OutputSpeed::Low, OutputInit::Low>
                                outputOD{};
        static constexpr Set    set{};
        static constexpr Clear  clear{};
        static constexpr Toggle toggle{};
    }   // namespace Action

    template<int I>
    struct HwPort {
        static constexpr int value = I;
        using type                 = HwPort<I>;
    };

    template<int I>
    struct Pin {
        static constexpr int value = I;
        using type                 = Pin<I>;
    };

    enum class PortAccess {
        defaultMode,
        setClear,
        toggle,
        exclusiveMask,
        sharedMask,
        readModifyWrite
    };

    template<PortAccess Access, typename... Ts>
    struct Port {
        using type = Port<Access, Ts...>;
    };

    namespace Access {
        static constexpr MPL::Value<PortAccess, PortAccess::defaultMode>
          defaultMode{};   // this will try to select the
                           // best mode for the chip used
        static constexpr MPL::Value<PortAccess, PortAccess::setClear>        setClear{};
        static constexpr MPL::Value<PortAccess, PortAccess::toggle>          toggle{};
        static constexpr MPL::Value<PortAccess, PortAccess::exclusiveMask>   exclusiveMask{};
        static constexpr MPL::Value<PortAccess, PortAccess::sharedMask>      sharedMask{};
        static constexpr MPL::Value<PortAccess, PortAccess::readModifyWrite> readModifyWrite{};
    }   // namespace Access

    // Io Pin which is not used
    template<typename = void>
    struct NotUsed {};

}   // namespace Io

// Pin location needs to live in Register in order for the factory functions to be found by ADL
namespace Register {
    template<int Port, int Pin>
    struct PinLocation {
        using type = PinLocation<Port, Pin>;
    };
}   // namespace Register

namespace Io {
    // A GPIO as a Startup resource (kvasir/StartUp/Resources.hpp). Provided by whoever
    // configures the pad, which the chip layer derives from the peripheral's
    // initStepPinConfig, so nothing declares it; claimed by a driver that drives or reads a
    // pin it does not configure (a reset line, a chip select, an interrupt input), which is
    // how "the pin is not in HW::PinConfig" becomes a build error. Several drivers may claim
    // one pin (an interrupt and a poll of the same input), and the pad is chip-wide, so the
    // provider may sit in either core's list.
    struct PinTag {
        static constexpr bool     sharedClaim = true;
        static constexpr bool     coreLocal   = false;
        static constexpr unsigned keyArity    = 2;
    };

    template<int Port, int Pin>
    using PinResource = Startup::Resource<PinTag, Port, Pin>;

    namespace Detail {
        template<typename T>
        struct PinClaimOf {
            using type = brigand::list<>;   // NotUsed<>, or any other non-pin: nothing
        };

        template<int Port, int Pin>
        struct PinClaimOf<Register::PinLocation<Port, Pin>> {
            using type = brigand::list<PinResource<Port, Pin>>;
        };
    }   // namespace Detail

    // `using Claims = Kvasir::Io::PinClaims<CsPin, RstPin>;` in a driver that is handed pins.
    template<typename... Pins>
    using PinClaims = brigand::flatten<brigand::list<typename Detail::PinClaimOf<Pins>::type...>>;
}   // namespace Io
}   // namespace Kvasir
