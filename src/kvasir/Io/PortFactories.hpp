#pragma once
#include "Mpl/Utility.hpp"
#include "PinFactories.hpp"

namespace Kvasir { namespace Io {
    template<typename T,
             typename... Ts>
    constexpr MPL::EnableIfT<Detail::IsPinLoaction<T>::value,
                             Port<PortAccess::defaultMode,
                                  T,
                                  Ts...>>
    makePort(T,
             Ts...) {
        return {};
    }

    template<typename T,
             typename... Ts>
    constexpr MPL::DisableIfT<Detail::IsPinLoaction<T>::value,
                              Port<T::Value,
                                   Ts...>>
    makePort(T,
             Ts...) {
        return {};
    }

    namespace Detail {
        template<typename T>
        struct PortMakeOutput;
        template<PortAccess A, typename... Ts>
        struct PortMakeOutput<Port<A, Ts...>> : decltype(makeOutput(Ts{}...)){};
        template<typename T>
        using PortMakeOutputT = typename PortMakeOutput<T>::type;

        template<typename T>
        struct PortMakeInput;
        template<PortAccess A, typename... Ts>
        struct PortMakeInput<Port<A, Ts...>> : decltype(makeInput(Ts{}...)){};
        template<typename T>
        using PortMakeInputT = typename PortMakeInput<T>::type;

        //###### Traits that a chip file should specialize
        template<PortAccess A, unsigned PortNumber, unsigned PinMask, typename Value>
        struct WriteLiteralToSinglePort {
            static_assert(MPL::AlwaysFalse<Value>::value,
                          "this functionality is not supported by the included chip file");
            using type = int;   // SFINAE should not fail but should static assert
        };

        template<PortAccess A, typename PortNumbers, typename PinMasks, typename Values>
        struct WriteLiteralToDistributedPort {
            static_assert(MPL::AlwaysFalse<Values>::value,
                          "this functionality is not supported by the included chip file");
            using type = int;   // SFINAE should not fail but should static assert
        };

        template<typename TPort>
        struct WriteRuntimeToPort {
            static_assert(MPL::AlwaysFalse<TPort>::value,
                          "this functionality is not supported by the included chip file");
            using type = int;   // SFINAE should not fail but should static assert
        };

        template<typename TPort>
        using WriteRuntimeToPortT = typename WriteRuntimeToPort<TPort>::type;

        template<typename TPort>
        struct IsIsolated {
            static_assert(MPL::AlwaysFalse<TPort>::value,
                          "this functionality is not supported by the included chip file");
            using type = int;   // SFINAE should not fail but should static assert
        };

        template<typename TPort>
        struct IsSynchronous {
            static_assert(MPL::AlwaysFalse<TPort>::value,
                          "this functionality is not supported by the included chip file");
            using type = int;   // SFINAE should not fail but should static assert
        };

        namespace br = brigand;
        template<bool IsSingle, typename TPort, typename Value>
        struct WriteLiteralToPortHelper;

        template<PortAccess A, typename... Ts, typename Value>
        struct WriteLiteralToPortHelper<false, Port<A, Ts...>, Value>
          : WriteLiteralToDistributedPort<A, br::list<>, br::list<>, Value> {};

        template<PortAccess A, typename... Ts, typename Value>
        struct WriteLiteralToPortHelper<true, Port<A, Ts...>, Value>
          : WriteLiteralToSinglePort<A,
                                     AtT<GetPortNumbersT<br::list<Ts...>>, Int<0>>::value,
                                     0,
                                     Value> {};

        template<bool IsSingle, typename TPort, typename Value>
        using WriteLiteralToPortHelperT =
          typename WriteLiteralToPortHelper<IsSingle, TPort, Value>::type;

    }   // namespace Detail

    template<typename TPort>
    constexpr MPL::EnableIfT<Detail::IsPort<TPort>::value,
                             Detail::PortMakeOutputT<TPort>>
    makeOutput(TPort) {
        return {};
    }

    template<typename TPort>
    constexpr MPL::EnableIfT<Detail::IsPort<TPort>::value,
                             Detail::PortMakeInputT<TPort>>
    makeInput(TPort) {
        return {};
    }

    template<typename TPort,
             typename TValue>
    constexpr MPL::EnableIfT<Detail::IsPort<TPort>::value,
                             Detail::WriteLiteralToPortHelperT<Detail::IsSinglePort<TPort>::value,
                                                               TPort,
                                                               TValue>>
    write(TPort,
          TValue) {
        return {};
    }

    template<typename TPort>
    constexpr MPL::EnableIfT<Detail::IsPort<TPort>::value,
                             Detail::WriteRuntimeToPortT<TPort>>
    write(TPort,
          unsigned value) {
        return Detail::WriteRuntimeToPortT<TPort>{value};
    }

    template<typename T>
    constexpr bool isIsolated(T) {
        return Detail::IsIsolated<T>::value;
    }

    template<typename T>
    constexpr bool isSynchronous(T) {
        return Detail::IsSynchronous<T>::value;
    }
}}   // namespace Kvasir::Io
