#pragma once
#include "kvasir/Mpl/Types.hpp"

namespace Kvasir {
namespace Io {
    enum class PullConfiguration { PullNone, PullUp, PullDown };
    enum class OutputType { PushPull, OpenDrain };

    enum class OutputSpeed { Low, Medium, High, VeryHigh };

    namespace Action {
        template<PullConfiguration PC>
        struct Input {};

        template<OutputType OT, OutputSpeed OS>
        struct Output {};

        struct Set {};

        struct Clear {};

        struct Toggle {};

        struct Read {};

        struct Analog {};

        template<int               I,
                 OutputType        OT = OutputType::PushPull,
                 OutputSpeed       OS = OutputSpeed::Low,
                 PullConfiguration PC = PullConfiguration::PullNone>
        struct PinFunction {
            static constexpr int value = I;
        };

        static constexpr Input<PullConfiguration::PullNone>              input{};
        static constexpr Input<PullConfiguration::PullNone>              inputPN{};
        static constexpr Input<PullConfiguration::PullUp>                inputPU{};
        static constexpr Input<PullConfiguration::PullDown>              inputPD{};
        static constexpr Output<OutputType::PushPull, OutputSpeed::Low>  output{};
        static constexpr Output<OutputType::PushPull, OutputSpeed::Low>  outputPP{};
        static constexpr Output<OutputType::OpenDrain, OutputSpeed::Low> outputOD{};
        static constexpr Set                                             set{};
        static constexpr Clear                                           clear{};
        static constexpr Toggle                                          toggle{};
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
}   // namespace Kvasir
