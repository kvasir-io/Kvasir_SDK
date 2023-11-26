#pragma once
#include "Utility.hpp"

namespace Kvasir { namespace Register {
    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsWriteLiteral<T>::value> isolated(T) {}
}}   // namespace Kvasir::Register
