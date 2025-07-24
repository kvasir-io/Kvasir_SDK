#pragma once
#include "brigand.hpp"

namespace Kvasir { namespace MPL {

    namespace Detail {
        // in some cases we need a type that the user will never use
        struct InternalUseOnly {};
    }   // namespace Detail

    // used to move the point of compilation to the point of instantiation of the parameter
    // use this class to static assert only if an implausible specialization is actually instantiated
    template<typename>
    struct AlwaysFalse {
        enum { value = 0 };
    };

    template<>
    struct AlwaysFalse<Detail::InternalUseOnly> {
        enum { value = 1 };
    };

    template<typename T>
    struct Return {
        using type = T;
    };

    // Wrapper for integral type template parameters
    template<typename T, T I>
    struct Value {
        static constexpr T value{I};
        using type = Value<T, I>;
    };

    template<bool B>
    using Bool      = Value<bool, B>;
    using TrueType  = Bool<true>;
    using FalseType = Bool<false>;

    template<int I>
    using Int = Value<int, I>;

    template<unsigned I>
    using Unsigned = Value<unsigned, I>;

    template<typename... Ts>
    constexpr brigand::list<Ts...> list(Ts...) {
        return brigand::list<Ts...>{};
    }

    // wrapper for template template parameters
    template<template<typename...> class T>
    struct Template {
        template<typename... Ts>
        using apply = T<Ts...>;
    };

    template<typename T, typename... Ts>
    using ApplyTemplateT = typename T::template apply<Ts...>::type;

    template<typename T, typename U>
    struct Pair {
        using type   = Pair<T, U>;
        using First  = T;
        using Second = U;
    };

    template<typename T>
    using PairFirst = typename T::First;
    template<typename T>
    using PairSecond = typename T::Second;

    // used for debugging
    template<typename...>
    struct Print;

}}   // namespace Kvasir::MPL
