#pragma once
#include "Utility.hpp"

namespace Kvasir { namespace Register {
    //##### Thread synchronization factories #######
    // warning these are still a work in progress!

    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsWriteLiteral<T>::value> atomic(T) {}
    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsWriteRuntime<T>::value> atomic(T) {}
    template<typename T, typename U, typename... Ts>
    constexpr MPL::EnableIfT<Detail::IsWriteLiteral<T>::value> atomic(T) {}
    template<typename T, typename U, typename... Ts>
    constexpr MPL::EnableIfT<Detail::IsWriteRuntime<T>::value> atomic(T) {}

}}   // namespace Kvasir::Register
