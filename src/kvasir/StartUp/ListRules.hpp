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
//   SingleIsrPriority     an entry with one context for every ISR (singleIsrPriorityLevel)
//                         needs every enabled interrupt that may log at one priority level:
//                         at two levels the higher one preempts the lower mid-record; levels
//                         the entry declares silent (silentIsrPriorityLevels) do not count
//   IsrPrioritiesServed   an entry with one context per level (isrPriorityLevels) needs
//                         every enabled interrupt at a level it serves or declares silent
//   IsrLevelsKnown        both need every enabled interrupt's level: a priority written in a
//                         form the core layer cannot read (unknownIsrLevel) proves nothing
//   IsrContractHolds      the three for one entry, over every core's interrupts together, or
//                         core by core for an entry whose contexts are per core
//                         (perCoreIsrContexts)
//
// The levels are the ones the init steps write, in the order Startup applies them; a priority
// changed at run time is invisible to these rules.

#include "kvasir/Mpl/Types.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <array>
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

    // The ISR-context contract an entry may declare (uc_log's IsrPolicy does): the levels it
    // serves, the levels it takes as never logging, or that it needs a single level. Plain
    // std::array<int, N> / bool members, so declaring them needs nothing from Kvasir.
    template<typename T>
    constexpr bool declaresIsrLevels = requires { T::isrPriorityLevels; };

    template<typename T>
    constexpr bool declaresSingleIsrLevel = requires { T::singleIsrPriorityLevel; };

    template<typename T>
    constexpr bool declaresIsrContract = declaresIsrLevels<T> || declaresSingleIsrLevel<T>;

    // An entry whose ISR contexts are separate per core (uc_log's MulticoreRttComBackend): an
    // ISR on one core never preempts a record on the other's, so its contract is held to each
    // core's interrupts on their own.
    template<typename T>
    constexpr bool declaresPerCoreIsrContexts = requires { requires T::perCoreIsrContexts; };

    template<typename T>
    constexpr auto silentIsrLevelsOf() {
        if constexpr(requires { T::silentIsrPriorityLevels; }) {
            return T::silentIsrPriorityLevels;
        } else {
            return std::array<int, 0>{};
        }
    }

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

    // ---- interrupt priorities ---------------------------------------------------------------
    namespace Detail {
        template<typename List>
        struct PrioritiesOfActions;

        template<typename... As>
        struct PrioritiesOfActions<brigand::list<As...>> {
            using type = brigand::flatten<brigand::list<typename PriorityOfAction<As>::type...>>;
        };

        // The IsrPriority<I, L> a peripheral's interrupt-config and periphery-enable steps set.
        template<typename T>
        using ConfigPrioritiesOf =
          typename PrioritiesOfActions<brigand::flatten<typename InterruptConfigOf<T>::type>>::type;

        template<typename T>
        using EnablePrioritiesOf =
          typename PrioritiesOfActions<brigand::flatten<typename PeripheryEnableOf<T>::type>>::type;

        // The level the last write for index I sets, else 0: the reset value, the highest.
        template<int I,
                 typename... Ps>
        constexpr int levelOf(brigand::list<Ps...>) {
            constexpr int indexes[]{Ps::index..., 0};
            constexpr int levels[]{Ps::level..., 0};
            int           level = 0;
            for(std::size_t i = 0; i < sizeof...(Ps); ++i) {
                if(indexes[i] == I) { level = levels[i]; }
            }
            return level;
        }

        template<typename Enabled, typename Set>
        struct WithLevels;

        template<typename... Es, typename Set>
        struct WithLevels<brigand::list<Es...>, Set> {
            using type = brigand::list<IsrPriority<Es::value, levelOf<Es::value>(Set{})>...>;
        };

        template<auto Set>
        constexpr bool inLevelSet(int level) {
            for(auto const l : Set) {
                if(l == level) { return true; }
            }
            return false;
        }

        // A level the single-level rule counts: known (IsrLevelsKnown reports the rest) and
        // not silent.
        template<auto Silent>
        constexpr bool countsAsLevel(int level) {
            return level != unknownIsrLevel && !inLevelSet<Silent>(level);
        }

        // How many distinct levels the entries use that countsAsLevel.
        template<auto Silent,
                 typename... Ps>
        constexpr std::size_t distinctLevels(brigand::list<Ps...>) {
            constexpr int         levels[]{Ps::level..., 0};
            constexpr std::size_t n        = sizeof...(Ps);
            std::size_t           distinct = 0;
            for(std::size_t i = 0; i < n; ++i) {
                if(!countsAsLevel<Silent>(levels[i])) { continue; }
                bool seen = false;
                for(std::size_t j = 0; j < i; ++j) {
                    if(levels[j] == levels[i]) { seen = true; }
                }
                if(!seen) { ++distinct; }
            }
            return distinct;
        }
    }   // namespace Detail

    // Every enabled index of the list with the level it is left at: brigand::list<IsrPriority<I,
    // L>...>, in the order of EnabledInterruptsIn. Startup applies every entry's
    // initStepInterruptConfig and then every entry's initStepPeripheryEnable, so of two writes of
    // one index's priority the one in the later phase, or later in the list, sets the level.
    template<typename List>
    struct IsrPrioritiesIn;

    template<typename... Ts>
    struct IsrPrioritiesIn<brigand::list<Ts...>> {
        using Enabled = typename EnabledInterruptsIn<brigand::list<Ts...>>::type;
        using Set     = brigand::flatten<
          brigand::list<Detail::ConfigPrioritiesOf<Ts>..., Detail::EnablePrioritiesOf<Ts>...>>;
        using type = typename Detail::WithLevels<Enabled, Set>::type;
    };

    // Silent: a std::array<int, N> of the levels that do not count; Prios: an IsrPrioritiesIn
    // result (one core's, or several cores' appended). Unknown levels are IsrLevelsKnown's.
    template<auto Silent, typename Prios>
    struct SingleIsrPriority;

    template<auto Silent, typename... Ps>
    struct SingleIsrPriority<Silent, brigand::list<Ps...>>
      : std::bool_constant<(Detail::distinctLevels<Silent>(brigand::list<Ps...>{}) <= 1)> {};

    // Served: a std::array<int, N> of the levels the entry has a context for. Unknown levels
    // are IsrLevelsKnown's.
    template<auto Served, auto Silent, typename Prios>
    struct IsrPrioritiesServed;

    template<auto Served, auto Silent, typename... Ps>
    struct IsrPrioritiesServed<Served, Silent, brigand::list<Ps...>>
      : std::bool_constant<((Ps::level == unknownIsrLevel || Detail::inLevelSet<Served>(Ps::level)
                             || Detail::inLevelSet<Silent>(Ps::level))
                            && ...)> {};

    // Every enabled index at a level the core layer could read.
    template<typename Prios>
    struct IsrLevelsKnown;

    template<typename... Ps>
    struct IsrLevelsKnown<brigand::list<Ps...>>
      : std::bool_constant<((Ps::level != unknownIsrLevel) && ...)> {};

    // The offending indexes, for the reports: the enabled indexes at a known level that is
    // neither served nor silent, every counted index when more than one counted level is in
    // use (each one is part of the conflict), and the indexes at an unknown level.
    template<auto Served, auto Silent, typename Prios>
    struct UnservedIsrIndexes;

    template<auto Served, auto Silent, typename... Ps>
    struct UnservedIsrIndexes<Served, Silent, brigand::list<Ps...>> {
        using type = brigand::flatten<brigand::list<
          Detail::IndexIf<!(Ps::level == unknownIsrLevel || Detail::inLevelSet<Served>(Ps::level)
                            || Detail::inLevelSet<Silent>(Ps::level)),
                          Ps::index>...>>;
    };

    template<auto Silent, typename Prios>
    struct MultiLevelIsrIndexes;

    template<auto Silent, typename... Ps>
    struct MultiLevelIsrIndexes<Silent, brigand::list<Ps...>> {
        static constexpr bool conflict = Detail::distinctLevels<Silent>(brigand::list<Ps...>{}) > 1;
        using type                     = brigand::flatten<brigand::list<
          Detail::IndexIf<(conflict && Detail::countsAsLevel<Silent>(Ps::level)), Ps::index>...>>;
    };

    template<typename Prios>
    struct UnknownIsrLevelIndexes;

    template<typename... Ps>
    struct UnknownIsrLevelIndexes<brigand::list<Ps...>> {
        using type = brigand::flatten<
          brigand::list<Detail::IndexIf<(Ps::level == unknownIsrLevel), Ps::index>...>>;
    };

    // The IsrPrioritiesIn lists entry T's contract is held to, from the lists of every core
    // (brigand::list<primary list, each SecondaryCore's list...>): one per core for an entry
    // with per-core contexts, else one of every core's interrupts together, because an entry
    // the cores share is one set of contexts for all of them.
    template<typename T, typename CoreLists>
    struct IsrPriorityGroupsFor;

    template<typename T, typename... Ls>
    struct IsrPriorityGroupsFor<T, brigand::list<Ls...>> {
        using type = std::conditional_t<
          declaresPerCoreIsrContexts<T>,
          brigand::list<typename IsrPrioritiesIn<Ls>::type...>,
          brigand::list<brigand::flatten<brigand::list<typename IsrPrioritiesIn<Ls>::type...>>>>;
    };

    namespace Detail {
        template<typename T,
                 typename Prios>
        constexpr bool contractHoldsOn() {
            if(!IsrLevelsKnown<Prios>::value) { return false; }
            if constexpr(declaresIsrLevels<T>) {
                return IsrPrioritiesServed<T::isrPriorityLevels, silentIsrLevelsOf<T>(), Prios>::
                  value;
            } else {
                return SingleIsrPriority<silentIsrLevelsOf<T>(), Prios>::value;
            }
        }

        template<typename T,
                 typename... Gs>
        constexpr bool contractHoldsOnAll(brigand::list<Gs...>) {
            return (contractHoldsOn<T, Gs>() && ...);
        }
    }   // namespace Detail

    // Whether entry T's ISR-context contract holds on the interrupts of the core lists: every
    // level known, and every level served (isrPriorityLevels) or at most one counted level
    // (singleIsrPriorityLevel), on each group of IsrPriorityGroupsFor. An entry without a
    // contract passes, and so does one with both members, which Startup rejects on its own.
    template<typename T, typename CoreLists>
    struct IsrContractHolds
      : std::bool_constant<
          !declaresIsrContract<T> || (declaresIsrLevels<T> && declaresSingleIsrLevel<T>)
          || Detail::contractHoldsOnAll<T>(typename IsrPriorityGroupsFor<T, CoreLists>::type{})> {};

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
