#pragma once
#include "Types.hpp"

#include <type_traits>

namespace Kvasir { namespace Io { namespace Detail {
    template<typename T>
    struct IsPinLocation : std::false_type {};

    template<int Port, int Pin>
    struct IsPinLocation<PinLocation<Port, Pin>> : std::true_type {};

    template<typename T, typename U>
    struct PinLocationLess;

    template<int PortL, int PinL, int PortR, int PinR>
    struct PinLocationLess<PinLocation<PortL, PinL>, PinLocation<PortR, PinR>>
      : std::bool_constant<(PortL == PortR ? PinL < PinR : PortL < PortR)> {};

}}}   // namespace Kvasir::Io::Detail

namespace Kvasir { namespace Io {
    /// Whether two pin locations name the same pin; false for anything that is no PinLocation (NotUsed<>).
    template<int LPort,
             int LPin,
             int RPort,
             int RPin>
    constexpr bool pinLocationEqual(PinLocation<LPort,
                                                LPin> l,
                                    PinLocation<RPort,
                                                RPin> r) {
        return !Detail::PinLocationLess<std::decay_t<decltype(l)>, std::decay_t<decltype(r)>>::value
            && !Detail::PinLocationLess<std::decay_t<decltype(r)>,
                                        std::decay_t<decltype(l)>>::value;
    }

    template<typename T,
             typename U>
    constexpr bool pinLocationEqual(T,
                                    U) {
        return false;
    }
}}   // namespace Kvasir::Io
