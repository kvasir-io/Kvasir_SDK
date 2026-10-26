#pragma once

// Fold-based helpers over brigand::list, for the Startup headers' compile-time checks.
//
// One implementation of each list algorithm the Startup headers need, instead of a copy per
// header. Fold expressions and an accumulating specialisation instead of brigand's
// algorithms: they compile faster, and every result is the plain brigand::list a copy made
// before. Order is kept everywhere.
//
//   count<T, L>          how often T is in L
//   contains<T, L>       count<T, L> != 0
//   Dedup<L>             L without repeats, the FIRST occurrence kept
//   Filter<Pred, L>      the elements of L for which Pred holds
//   FirstWhere<Pred, Ts...>::type / FirstWhereT
//                        the first T for which Pred holds, or void
//   Expand<F, L>         F<Ts...> for L = brigand::list<Ts...>
//
// A predicate is a type with `template<class T> static constexpr bool holds`, so it can carry
// bound parameters as its own template arguments. Holds<Trait> adapts a `Trait<T>::value`
// class template, NotP<Pred> negates a predicate.

#include "kvasir/Mpl/brigand.hpp"

#include <cstddef>
#include <type_traits>

namespace Kvasir { namespace MPL {

    template<template<typename> class Trait>
    struct Holds {
        template<typename T>
        static constexpr bool holds = Trait<T>::value;
    };

    template<typename Pred>
    struct NotP {
        template<typename T>
        static constexpr bool holds = !Pred::template holds<T>;
    };

    template<typename T, typename List>
    inline constexpr std::size_t count = 0;

    template<typename T, typename... Ts>
    inline constexpr std::size_t count<T, brigand::list<Ts...>>
      = (std::size_t{0} + ... + std::size_t{std::is_same_v<T, Ts>});

    template<typename T, typename List>
    inline constexpr bool contains = count<T, List> != 0;

    namespace Detail {
        template<typename Out, typename In>
        struct DedupImpl;

        template<typename... Os>
        struct DedupImpl<brigand::list<Os...>, brigand::list<>> {
            using type = brigand::list<Os...>;
        };

        template<typename... Os, typename T, typename... Ts>
        struct DedupImpl<brigand::list<Os...>, brigand::list<T, Ts...>>
          : DedupImpl<std::conditional_t<(std::is_same_v<T, Os> || ...),
                                         brigand::list<Os...>,
                                         brigand::list<Os..., T>>,
                      brigand::list<Ts...>> {};

        template<typename Pred, typename Out, typename In>
        struct FilterImpl;

        template<typename Pred, typename... Os>
        struct FilterImpl<Pred, brigand::list<Os...>, brigand::list<>> {
            using type = brigand::list<Os...>;
        };

        template<typename Pred, typename... Os, typename T, typename... Ts>
        struct FilterImpl<Pred, brigand::list<Os...>, brigand::list<T, Ts...>>
          : FilterImpl<Pred,
                       std::conditional_t<Pred::template holds<T>,
                                          brigand::list<Os..., T>,
                                          brigand::list<Os...>>,
                       brigand::list<Ts...>> {};

        template<template<typename...> class F, typename List>
        struct ExpandImpl;

        template<template<typename...> class F, typename... Ts>
        struct ExpandImpl<F, brigand::list<Ts...>> {
            using type = F<Ts...>;
        };
    }   // namespace Detail

    template<typename List>
    using Dedup = typename Detail::DedupImpl<brigand::list<>, List>::type;

    template<typename Pred, typename List>
    using Filter = typename Detail::FilterImpl<Pred, brigand::list<>, List>::type;

    template<typename Pred, typename... Ts>
    struct FirstWhere {
        using type = void;
    };

    template<typename Pred, typename T, typename... Ts>
    struct FirstWhere<Pred, T, Ts...>
      : std::
          conditional_t<Pred::template holds<T>, std::type_identity<T>, FirstWhere<Pred, Ts...>> {};

    template<typename Pred, typename... Ts>
    using FirstWhereT = typename FirstWhere<Pred, Ts...>::type;

    template<template<typename...> class F, typename List>
    using Expand = typename Detail::ExpandImpl<F, List>::type;

}}   // namespace Kvasir::MPL
