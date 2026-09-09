#pragma once

// Rules over the shape of a Startup list that need no chip and no register: the list itself
// is wrong, not the hardware it describes. Each is a std::bool_constant; Startup and
// SecondaryCore turn them into static_asserts, tests/startup_list_tests.cpp checks them both
// ways on fake lists. Anything that needs the chip (the interrupt range, what a SecondaryCore
// is) comes in as a template argument so this header compiles on the host.
//
//   NoDuplicateEntry      the same type twice in one list runs its init steps twice
//   AllArePeripherals     a type that is not a peripheral (a Config listed instead of its
//                         driver, a typo'd alias) contributes nothing, and nothing says so
//   AtMostOneSecondary    the chip has one other core
//   NoLaunchTimeoutIn     a LaunchTimeout outside a SecondaryCore's list is ignored
//   NoClockSettingsIn     a second ClockSettings among the peripherals never runs its clock
//                         init, or fights the real one with its init steps
//   IsrIndexesValid       an Isr on an index the chip's table does not have is silently
//                         absent from it: the line fires into the unhandled handler
//   EnabledLinesHandled   an init step enables an interrupt line for which nothing in the
//                         same list installs an Isr: the line fires into the unhandled
//                         handler at the first event
//   DependenciesPrecedeLaunch
//                         a SecondaryCore's peripheral that needs a core 0 peripheral
//                         (ListedOn<X, 0>, Resources.hpp) needs X's init to have run before
//                         the launch: X before the SecondaryCore in the primary list

#include "kvasir/Mpl/Types.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <cstddef>
#include <type_traits>

namespace Kvasir { namespace Startup { namespace ListRules {

    namespace Detail {
        template<typename T,
                 typename... Ts>
        constexpr std::size_t countSame(brigand::list<Ts...>) {
            return (std::size_t{0} + ... + std::size_t{std::is_same_v<T, Ts>});
        }

        template<typename Traits,
                 int I>
        constexpr bool isrIndexInTable() {
            if(I < Traits::begin || I >= Traits::end) { return false; }
            if constexpr(requires { Traits::disabled; }) {
                for(auto const d : Traits::disabled) {
                    if(d == I) { return false; }
                }
            }
            return true;
        }
    }   // namespace Detail

    // ---- what makes a type a Startup entry ---------------------------------------------
    // The members Startup, SecondaryCore and the resource check look for. A type with none
    // of them is inert in a list, which is never what listing it meant.
    template<typename T>
    constexpr bool hasInitSteps
      = requires { T::earlyInit; } || requires { T::powerClockEnable; }
     || requires { T::initStepPinConfig; } || requires { T::initStepPeripheryConfig; }
     || requires { T::initStepInterruptConfig; } || requires { T::initStepPeripheryEnable; };

    template<typename T>
    constexpr bool hasIsr = requires { typename T::Isr; } || requires { T::isr; };

    template<typename T>
    constexpr bool hasRuntimeHooks
      = requires { T::runtimeInit(); } || requires { T::preEnableRuntimeInit(); } || requires {
            T::primaryPrepare();
        } || requires { T::primarySync(); } || requires { T::secondarySync(); };

    template<typename T>
    constexpr bool hasResources
      = requires { typename T::Provides; } || requires { typename T::Claims; };

    // isLaunchTimeout: LaunchTimeout; startupCore: a peripheral that names its core;
    // isStartupEntry: Using<...> and anything else that is listed for the checks alone.
    template<typename T>
    constexpr bool hasMarker = requires { T::isLaunchTimeout; } || requires { T::startupCore; }
                            || requires { T::isStartupEntry; };

    template<typename T>
    constexpr bool isPeripheral
      = hasInitSteps<T> || hasIsr<T> || hasRuntimeHooks<T> || hasResources<T> || hasMarker<T>;

    // ---- the rules ---------------------------------------------------------------------
    template<typename List>
    struct NoDuplicateEntry;

    template<typename... Ts>
    struct NoDuplicateEntry<brigand::list<Ts...>>
      : std::bool_constant<((Detail::countSame<Ts>(brigand::list<Ts...>{}) == 1) && ...)> {};

    // IsSecondaryCore: the chip-agnostic predicate from StartUp.hpp, passed in so a test can
    // substitute its own.
    template<template<typename> class IsSecondaryCore, typename List>
    struct AllArePeripherals;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct AllArePeripherals<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<((isPeripheral<Ts> || IsSecondaryCore<Ts>::value) && ...)> {};

    template<template<typename> class IsSecondaryCore, typename List>
    struct AtMostOneSecondary;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct AtMostOneSecondary<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<((std::size_t{0} + ... + std::size_t{IsSecondaryCore<Ts>::value})
                            <= 1)> {};

    template<template<typename> class IsSecondaryCore, typename List>
    struct NoSecondaryIn;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct NoSecondaryIn<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<(!IsSecondaryCore<Ts>::value && ...)> {};

    template<typename List>
    struct NoLaunchTimeoutIn;

    template<typename... Ts>
    struct NoLaunchTimeoutIn<brigand::list<Ts...>>
      : std::bool_constant<(!requires {
        Ts::isLaunchTimeout; } && ...)> {};

    template<typename List>
    struct NoClockSettingsIn;

    template<typename... Ts>
    struct NoClockSettingsIn<brigand::list<Ts...>>
      : std::bool_constant<(
          !(requires {
        Ts::coreClockInit(); } || requires {
            Ts::peripheryClockInit(); })
          && ...)> {};

    // Traits: the chip's InterruptOffsetTraits (begin, end, disabled); IsrList: Nvic::Isr
    // types, each with an IType::value.
    template<typename Traits, typename IsrList>
    struct IsrIndexesValid;

    template<typename Traits, typename... Is>
    struct IsrIndexesValid<Traits, brigand::list<Is...>>
      : std::bool_constant<(Detail::isrIndexInTable<Traits, Is::IType::value>() && ...)> {};

    namespace Detail {
        // The X of a ListedOn<X, 0> claim; void for any other resource.
        template<typename R>
        struct ListedOnCore0 {
            using type = void;
        };

        template<typename X>
        struct ListedOnCore0<Kvasir::Startup::Detail::ResourceT<ListedOnTag<X>, 0>> {
            using type = X;
        };

        template<typename X,
                 typename... Ts>
        constexpr int positionOf() {
            constexpr bool same[]{std::is_same_v<X, Ts>..., false};
            for(int i = 0; i < static_cast<int>(sizeof...(Ts)); ++i) {
                if(same[i]) { return i; }
            }
            return -1;
        }

        // An X that is not in the primary list at all is dependenciesListed's finding, not
        // this one's, so -1 passes here.
        template<int Launch,
                 typename... Ts,
                 typename... Rs>
        constexpr bool claimsPrecede(brigand::list<Rs...>) {
            return ((std::is_void_v<typename ListedOnCore0<Rs>::type>
                     || positionOf<typename ListedOnCore0<Rs>::type, Ts...>() < Launch)
                    && ...);
        }

        template<template<typename> class IsSecondaryCore,
                 template<typename> class SecondaryPeripherals,
                 typename List,
                 typename Seq>
        struct DependenciesPrecedeLaunchImpl;

        template<template<typename> class IsSecondaryCore,
                 template<typename> class SecondaryPeripherals,
                 typename... Ts,
                 std::size_t... Is>
        struct DependenciesPrecedeLaunchImpl<IsSecondaryCore,
                                             SecondaryPeripherals,
                                             brigand::list<Ts...>,
                                             std::index_sequence<Is...>>
          : std::bool_constant<((!IsSecondaryCore<Ts>::value
                                 || claimsPrecede<static_cast<int>(Is), Ts...>(
                                   typename Kvasir::Startup::Detail::ClaimsIn<
                                     typename SecondaryPeripherals<Ts>::type>::type{}))
                                && ...)> {};
    }   // namespace Detail

    // ---- enabled without a handler --------------------------------------------------------
    namespace Detail {
        template<typename T>
        struct InterruptConfigOf {
            using type = brigand::list<>;
        };

        template<typename T>
            requires requires { T::initStepInterruptConfig; }
        struct InterruptConfigOf<T> {
            using type = typename Kvasir::Startup::Detail::AsList<
              std::remove_cvref_t<decltype(T::initStepInterruptConfig)>>::type;
        };

        template<typename T>
        struct PeripheryEnableOf {
            using type = brigand::list<>;
        };

        template<typename T>
            requires requires { T::initStepPeripheryEnable; }
        struct PeripheryEnableOf<T> {
            using type = typename Kvasir::Startup::Detail::AsList<
              std::remove_cvref_t<decltype(T::initStepPeripheryEnable)>>::type;
        };

        template<typename List>
        struct InterruptsOfActions;

        template<typename... As>
        struct InterruptsOfActions<brigand::list<As...>> {
            using type = brigand::flatten<brigand::list<typename InterruptOfAction<As>::type...>>;
        };

        // The indexes a peripheral's enable phases turn on.
        template<typename T>
        struct EnabledInterruptsOf {
            using Actions = brigand::flatten<brigand::append<typename InterruptConfigOf<T>::type,
                                                             typename PeripheryEnableOf<T>::type>>;
            using type    = typename InterruptsOfActions<Actions>::type;
        };

        template<int I,
                 typename... Is>
        constexpr bool anyIsrOn(brigand::list<Is...>) {
            return (false || ... || (Is::IType::value == I));
        }
    }   // namespace Detail

    template<typename List>
    struct EnabledInterruptsIn;

    template<typename... Ts>
    struct EnabledInterruptsIn<brigand::list<Ts...>> {
        using type
          = brigand::flatten<brigand::list<typename Detail::EnabledInterruptsOf<Ts>::type...>>;
    };

    // Enabled: std::integral_constant<int, I> per enabled index (EnabledInterruptsIn);
    // IsrList: the list's Isr types, each with an IType::value.
    template<typename Enabled, typename IsrList>
    struct EnabledLinesHandled;

    template<typename... Es, typename IsrList>
    struct EnabledLinesHandled<brigand::list<Es...>, IsrList>
      : std::bool_constant<(Detail::anyIsrOn<Es::value>(IsrList{}) && ...)> {};

    // ---- the offending indexes of the interrupt rules, for the reports ----------------------
    // Each is a brigand::list<std::integral_constant<int, I>...>, empty when the rule holds.
    namespace Detail {
        template<int I,
                 typename... Is>
        constexpr std::size_t isrsOn(brigand::list<Is...>) {
            return (std::size_t{0} + ... + std::size_t{Is::IType::value == I});
        }

        template<bool Keep, int I>
        using IndexIf = std::
          conditional_t<Keep, brigand::list<std::integral_constant<int, I>>, brigand::list<>>;
    }   // namespace Detail

    template<typename IsrList>
    struct DuplicateIsrIndexes;

    template<typename... Is>
    struct DuplicateIsrIndexes<brigand::list<Is...>> {
        using type = brigand::flatten<brigand::list<
          Detail::IndexIf<(Detail::isrsOn<Is::IType::value>(brigand::list<Is...>{}) > 1),
                          Is::IType::value>...>>;
    };

    template<typename Traits, typename IsrList>
    struct InvalidIsrIndexes;

    template<typename Traits, typename... Is>
    struct InvalidIsrIndexes<Traits, brigand::list<Is...>> {
        using type = brigand::flatten<
          brigand::list<Detail::IndexIf<!Detail::isrIndexInTable<Traits, Is::IType::value>(),
                                        Is::IType::value>...>>;
    };

    template<typename Enabled, typename IsrList>
    struct UnhandledIndexes;

    template<typename... Es, typename IsrList>
    struct UnhandledIndexes<brigand::list<Es...>, IsrList> {
        using type = brigand::flatten<
          brigand::list<Detail::IndexIf<!Detail::anyIsrOn<Es::value>(IsrList{}), Es::value>...>>;
    };

    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename List>
    struct DependenciesPrecedeLaunch
      : Detail::DependenciesPrecedeLaunchImpl<
          IsSecondaryCore,
          SecondaryPeripherals,
          List,
          std::make_index_sequence<brigand::size<List>::value>> {};

}}}   // namespace Kvasir::Startup::ListRules
