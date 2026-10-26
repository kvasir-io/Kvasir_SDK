#pragma once
// The subset of brigand (https://github.com/edouarda/brigand, Boost Software License 1.0) that Kvasir uses; the
// rest of the library, the placeholder/lambda engine (_1, _2, lambda, apply) with it, was removed. What is left:
//
//   list<Ts...>                  the type list (list<Ts...>::type is itself)
//   uint32_t<V>                  std::integral_constant<std::uint32_t, V>
//   size<L>                      std::integral_constant<std::size_t, number of elements>
//   at_c<L, I>                   element I
//   front<L>                     element 0
//   push_front<L, Ts...>         L<Ts..., elements>
//   push_back<L, Ts...>          L<elements, Ts...>
//   wrap<L, W>                   W<elements>
//   append<Ls...>                the elements of all Ls in one list of the first one's template; append<> is
//                                list<>, append<T> is T; every argument after the first must be a type list
//   flatten<L>                   L with every element that is itself an L<...> replaced by its elements,
//                                recursively; an empty nested L<> leaves nothing
//
// L is any class template over types (`template<class...> class`), not only brigand::list. The results are
// those of the brigand facilities of the same name; tests/brigand_subset_tests.cpp pins them. Putting a removed
// facility back: write it here over these primitives (Mpl/List.hpp has count/contains/Dedup/Filter as folds).
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace brigand {
template<class... T>
struct list {
    using type = list<T...>;
};

template<std::uint32_t V>
using uint32_t = std::integral_constant<std::uint32_t, V>;

namespace detail {
    template<class L>
    struct size_impl;

    template<template<class...> class L, class... Ts>
    struct size_impl<L<Ts...>> {
        using type = std::integral_constant<std::size_t, sizeof...(Ts)>;
    };

    template<class L, std::size_t I>
    struct at_impl;

    template<template<class...> class L, class... Ts, std::size_t I>
    struct at_impl<L<Ts...>, I> {
        static_assert(I < sizeof...(Ts),
                      "brigand::at_c: index out of range");
        using type = Ts...[I];
    };

    template<class L>
    struct front_impl;

    template<template<class...> class L, class T, class... Ts>
    struct front_impl<L<T, Ts...>> {
        using type = T;
    };

    template<class L, class... T>
    struct push_front_impl;

    template<template<class...> class L, class... U, class... T>
    struct push_front_impl<L<U...>, T...> {
        using type = L<T..., U...>;
    };

    template<class L, class... T>
    struct push_back_impl;

    template<template<class...> class L, class... U, class... T>
    struct push_back_impl<L<U...>, T...> {
        using type = L<U..., T...>;
    };

    template<class L, template<class...> class W>
    struct wrap_impl;

    template<template<class...> class L, class... T, template<class...> class W>
    struct wrap_impl<L<T...>, W> {
        using type = W<T...>;
    };

    // append and flatten: verbatim from brigand (append pairwise, and 16 lists at a time)
    template<typename... Ts>
    struct append_impl {
        using type = brigand::list<>;
    };

    template<typename T>
    struct append_impl<T> {
        using type = T;
    };

    template<template<typename...> class L1,
             template<typename...> class L2,
             typename... T1s,
             typename... T2s,
             typename... Ts>
    struct append_impl<L1<T1s...>, L2<T2s...>, Ts...> : append_impl<L1<T1s..., T2s...>, Ts...> {};

    template<template<typename...> class L,
             template<typename...> class L1,
             template<typename...> class L2,
             template<typename...> class L3,
             template<typename...> class L4,
             template<typename...> class L5,
             template<typename...> class L6,
             template<typename...> class L7,
             template<typename...> class L8,
             template<typename...> class L9,
             template<typename...> class L10,
             template<typename...> class L11,
             template<typename...> class L12,
             template<typename...> class L13,
             template<typename...> class L14,
             template<typename...> class L15,
             template<typename...> class L16,
             typename... Ts,
             typename... T1s,
             typename... T2s,
             typename... T3s,
             typename... T4s,
             typename... T5s,
             typename... T6s,
             typename... T7s,
             typename... T8s,
             typename... T9s,
             typename... T10s,
             typename... T11s,
             typename... T12s,
             typename... T13s,
             typename... T14s,
             typename... T15s,
             typename... T16s,
             typename... Us>
    struct append_impl<L<Ts...>,
                       L1<T1s...>,
                       L2<T2s...>,
                       L3<T3s...>,
                       L4<T4s...>,
                       L5<T5s...>,
                       L6<T6s...>,
                       L7<T7s...>,
                       L8<T8s...>,
                       L9<T9s...>,
                       L10<T10s...>,
                       L11<T11s...>,
                       L12<T12s...>,
                       L13<T13s...>,
                       L14<T14s...>,
                       L15<T15s...>,
                       L16<T16s...>,
                       Us...>
      : append_impl<L<Ts...,
                      T1s...,
                      T2s...,
                      T3s...,
                      T4s...,
                      T5s...,
                      T6s...,
                      T7s...,
                      T8s...,
                      T9s...,
                      T10s...,
                      T11s...,
                      T12s...,
                      T13s...,
                      T14s...,
                      T15s...,
                      T16s...>,
                    Us...> {};

    template<template<class...> class L, class T>
    struct flatten_element_impl {
        using type = L<T>;
    };

    template<template<class...> class L, class... Ts>
    struct flatten_element_impl<L, L<Ts...>>
      : append_impl<typename flatten_element_impl<L, Ts>::type...> {};

    template<class L>
    struct flatten_impl {
        using type = L;
    };

    template<template<class...> class L, class... Ts>
    struct flatten_impl<L<Ts...>> : flatten_element_impl<L, L<Ts...>> {};
}   // namespace detail

template<class L>
using size = typename detail::size_impl<L>::type;

template<class L, std::size_t Index>
using at_c = typename detail::at_impl<L, Index>::type;

template<class L>
using front = typename detail::front_impl<L>::type;

template<class L, class... T>
using push_front = typename detail::push_front_impl<L, T...>::type;

template<class L, class... T>
using push_back = typename detail::push_back_impl<L, T...>::type;

template<class L, template<class...> class W>
using wrap = typename detail::wrap_impl<L, W>::type;

template<class... Ls>
using append = typename detail::append_impl<Ls...>::type;

template<class L>
using flatten = typename detail::flatten_impl<L>::type;
}   // namespace brigand
