#pragma once
#include "brigand.hpp"

#include <concepts>
#include <type_traits>

namespace Kvasir { namespace MPL {

    // The one dependent false (Util/Diagnostic.hpp's Check and the Startup reports use it too): a static_assert on
    // it fires only when the template around it is instantiated. A named `static constexpr bool x =
    // AlwaysFalse<T>::value;` makes clang print "due to requirement 'x'" instead of the template arguments.
    template<typename...>
    struct AlwaysFalse {
        static constexpr bool value = false;
    };

    // A compile-time integer: std::integral_constant, or a tag type derived from one (Nvic::Index, Io::HwPort,
    // Io::Pin). Read as T::value. Register's literal write() takes every IntegralConstant and checks the type in its
    // body: a narrower constraint would send a constant that misses it into the runtime overload through
    // integral_constant's implicit conversion.
    template<typename T>
    concept IntegralConstant = requires {
        typename T::value_type;
        T::value;
    } && std::derived_from<T, std::integral_constant<typename T::value_type, T::value>>;

    template<typename... Ts>
    constexpr brigand::list<Ts...> list(Ts...) {
        return brigand::list<Ts...>{};
    }

}}   // namespace Kvasir::MPL
