#pragma once
// A GPIO as a Startup resource: the pin claims of a driver that is handed pins (`Kvasir::Io::PinClaims<CsPin, RstPin>`)
// and the resource the chip layer provides for each configured pad. Port layer (on StartUp/Resources.hpp); the pin
// vocabulary it names is Io/Types.hpp.
#include "kvasir/Io/Types.hpp"
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/StartUp/Resources.hpp"

namespace Kvasir { namespace Io {
    // A GPIO as a Startup resource. Provided by whoever
    // configures the pad, which the chip layer derives from the peripheral's
    // initStepPinConfig, so nothing declares it; claimed by a driver that drives or reads a
    // pin it does not configure (a reset line, a chip select, an interrupt input), which is
    // how "the pin is not in HW::PinConfig" becomes a build error. Several drivers may claim
    // one pin (an interrupt and a poll of the same input), and the pad is chip-wide, so the
    // provider may sit in either core's list.
    struct PinTag {
        static constexpr char const* describeAs  = "Io pin";   // "Io pin 0.25" in a compile error
        static constexpr bool        sharedClaim = true;
        static constexpr bool        coreLocal   = false;
        static constexpr unsigned    keyArity    = 2;
    };

    template<int Port, int Pin>
    using PinResource = Startup::Resource<PinTag, Port, Pin>;

    namespace Detail {
        template<typename T>
        struct PinClaimOf {
            using type = brigand::list<>;   // NotUsed<>, or any other non-pin: nothing
        };

        template<int Port, int Pin>
        struct PinClaimOf<PinLocation<Port, Pin>> {
            using type = brigand::list<PinResource<Port, Pin>>;
        };
    }   // namespace Detail

    // `using Claims = Kvasir::Io::PinClaims<CsPin, RstPin>;` in a driver that is handed pins.
    template<typename... Pins>
    using PinClaims = brigand::flatten<brigand::list<typename Detail::PinClaimOf<Pins>::type...>>;
}}   // namespace Kvasir::Io
