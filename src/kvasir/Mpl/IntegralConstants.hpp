#pragma once
#include "Types.hpp"

namespace Kvasir { namespace MPL { namespace IntegralConstants {
    static constexpr Unsigned<0> _0{};
    static constexpr Unsigned<1> _1{};
    static constexpr Unsigned<2> _2{};
    static constexpr Unsigned<3> _3{};
    static constexpr Unsigned<4> _4{};
    static constexpr Unsigned<5> _5{};
    static constexpr Unsigned<6> _6{};
    static constexpr Unsigned<7> _7{};
    static constexpr Unsigned<8> _8{};
    static constexpr Unsigned<9> _9{};

    template<unsigned L,
             unsigned R>
    constexpr inline Unsigned<(L * 10 + R)> operator,
      (Unsigned<L>,
       Unsigned<R>) {
        return {};
    }
}}}   // namespace Kvasir::MPL::IntegralConstants
