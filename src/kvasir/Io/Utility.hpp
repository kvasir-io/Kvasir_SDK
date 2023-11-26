#pragma once
#include "Types.hpp"
#include "kvasir/Mpl/Algorithm.hpp"

namespace Kvasir { namespace Io { namespace Detail {
    using namespace MPL;
    template<typename T>
    struct IsPinLoaction : FalseType {};
    template<int Port, int Pin>
    struct IsPinLoaction<Register::PinLocation<Port, Pin>> : TrueType {};

    template<typename T, typename U>
    struct PinLocationLess;
    template<int PortL, int PinL, int PortR, int PinR>
    struct PinLocationLess<Register::PinLocation<PortL, PinL>, Register::PinLocation<PortR, PinR>>
      : Bool<(PortL == PortR ? PinL < PinR : PortL < PortR)> {};
    using PinLocationLessP = Template<PinLocationLess>;

    template<int LPort, int LPin, int RPort, int RPin>
    constexpr bool
    PinLocationEqual(Register::PinLocation<LPort, LPin> l, Register::PinLocation<RPort, RPin> r) {
        return !Io::Detail::PinLocationLess<std::decay_t<decltype(l)>, std::decay_t<decltype(r)>>::
                 value
            && !Io::Detail::PinLocationLess<std::decay_t<decltype(r)>, std::decay_t<decltype(l)>>::
                 value;
    }

    template<typename T, typename U>
    constexpr bool PinLocationEqual(T, U) {
        return false;
    }

    template<typename T>
    struct IsPort : FalseType {};
    template<PortAccess A, typename... Ts>
    struct IsPort<Port<A, Ts...>> : TrueType {};

    template<typename T>
    struct GetHwPort;
    template<int Port, int Pin>
    struct GetHwPort<Register::PinLocation<Port, Pin>> : Int<Port> {};
    using GetHwPortP = Template<GetHwPort>;

    template<typename T, typename U>
    struct OnSamePort : FalseType {};
    template<int Port, int PinL, int PinR>
    struct OnSamePort<Register::PinLocation<Port, PinL>, Register::PinLocation<Port, PinR>>
      : TrueType {};
    using PortEqualP = Template<OnSamePort>;

    template<typename TList>
    using GetPortNumbersT
      = brigand::transform<UniqueT<SortT<TList, PinLocationLessP>, PortEqualP>, GetHwPortP>;

    template<typename T>
    struct IsSinglePort : FalseType {};
    template<PortAccess A, typename... Ts>
    struct IsSinglePort<Port<A, Ts...>>
      : Bool<(Size<GetPortNumbersT<brigand::list<Ts...>>>::value == 1)> {};

    template<typename T>
    struct IsDistributedPort : FalseType {};
    template<PortAccess A, typename... Ts>
    struct IsDistributedPort<Port<A, Ts...>>
      : Bool<(Size<GetPortNumbersT<brigand::list<Ts...>>>::value > 1)> {};

    template<typename T>
    struct GetAccess;
    template<PortAccess A, typename... Ts>
    struct GetAccess<Port<A, Ts...>> : Value<PortAccess, A> {};

}}}   // namespace Kvasir::Io::Detail
