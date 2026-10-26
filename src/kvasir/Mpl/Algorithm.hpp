#pragma once
#include "Types.hpp"

#include <type_traits>

namespace Kvasir { namespace MPL {
    namespace Detail {
        // default only reached when Ts is empty
        template<int I, typename TFind, typename... Ts>
        struct Find : std::integral_constant<int, -1> {};

        // TFind does not match first of Ts
        template<int I, typename TFind, typename T, typename... Ts>
        struct Find<I, TFind, T, Ts...> : Find<I + 1, TFind, Ts...> {};

        // TFind matches first of Ts
        template<int I, typename TFind, typename... Ts>
        struct Find<I, TFind, TFind, Ts...> : std::integral_constant<int, I> {};
    }   // namespace Detail

    // Find is std::integral_constant<int, -1> if type is not found, otherwise returns index
    template<typename TList, typename TToFind>
    struct Find {
        static_assert(AlwaysFalse<TToFind>::value,
                      "implausible parameters");
    };

    template<typename T, typename... Ts>
    struct Find<brigand::list<Ts...>, T> : Detail::Find<0, T, Ts...> {};

    template<typename TList, typename TToFind>
    using FindT = typename Find<TList, TToFind>::type;

}}   // namespace Kvasir::MPL
