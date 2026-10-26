#pragma once

// Rules over the shape of a Startup list that need no chip and no register: the list itself
// is wrong, not the hardware it describes. Each rule is an offender list (DuplicateEntries,
// NotPeripherals, ..., the *Indexes of the interrupt rules); the std::bool_constant names below
// are views of it (true when it is empty). Checks.hpp asserts them, one report per offender,
// for Startup and SecondaryCore alike; tests/startup_list_tests.cpp checks them both ways on
// fake lists. Anything that needs the chip (the interrupt range, what a SecondaryCore is)
// comes in as a template argument so this header compiles on the host.
//
//   NoDuplicateEntry      the same type twice in one list runs its init steps twice
//   HookShapesHold        an entry declares a runtime hook (Entry.hpp's Startup::Detail::RuntimeHook::All)
//                         that Startup cannot call - another return type, not static, or with
//                         parameters: it would never run, and a rendezvous hook would count as met
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
//   GuardBeforeReport     a BootGuard::Guard (isBootGuard) after Boot::Report (isBootReport): the report takes
//                         the panic and fault records before the guard reads them, and the guard judges every
//                         run healthy
//   HealthServiceAfterReport
//                         a Health::Service<> (isHealthService) before Boot::Report (isBootReport): the service's
//                         runtimeInit reports the run before's starvation, a line that follows the boot line
//   WatchdogBeforeHealthService
//                         a chip watchdog (isWatchdog: every Kvasir::Watchdog, Health/WatchdogFront.hpp) after a
//                         Health::Service<>: the service's runtimeInit holds the watchdog off, and one that its own
//                         runtimeInit arms later (an ungated SAM or STM32 watchdog) would count through the rest of
//                         the boot and the first turns; a gated one (isGatedWatchdog) has no runtimeInit and may
//                         come after
//   BootReportListed      a list with the user log backend names the boot report: an entry with isBootReport
//                         (Boot::Report, Runtime::Standard) or bootReportInMain (Boot::ReportInMain, a main that
//                         reports by hand). Without it a panic or fault record stays unreported
//   ChipEntryNotListed    an entry the chip adds to every primary list itself (chipEntry, Startup::ChipEntries)
//                         listed by hand: it would run twice (the RP chips' Clocks::DegradedBoot: two records, two
//                         tries a turn)
//
// The rules read a list's entries: a group entry (Entry.hpp) is its members, in its place.
// The levels are the ones the init steps write, in the order Startup applies them; a priority
// changed at run time is invisible to these rules.

#include "kvasir/Mpl/List.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/Util/Diagnostic.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace Kvasir { namespace Startup { namespace ListRules {

    namespace Detail {
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
    constexpr bool hasInitSteps = ::Kvasir::Startup::Detail::hasPhaseMember<T>;   // Entry.hpp

    template<typename T>
    constexpr bool hasIsr
      = requires { typename T::Isr; } || requires { typename T::SubIsrs; };   // SharedIsr.hpp

    // a declared hook makes an entry, whatever its shape: HookShapesHold then names the wrong one
    template<typename T>
    constexpr bool hasRuntimeHooks = ::Kvasir::Startup::Detail::RuntimeHook::DeclaresAny<T>::value
                                  || requires { typename T::Extends; };

    template<typename T>
    constexpr bool hasResources
      = requires { typename T::Provides; } || requires { typename T::Claims; };

    // isLaunchTimeout: LaunchTimeout; startupCore: a peripheral that names its core;
    // isStartupEntry: Using<...>, DrivenByHand<...> (Port/Extends.hpp) and anything else that is listed for the checks
    // alone.
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

    // An entry whose ISR contexts are separate per core (uc_log's RttLog on two cores): an
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

    namespace Detail {
        // A rule's offender, for its compile error, is MPL::FirstWhereT<Pred, Ts...> (Mpl/List.hpp)
        // over one of the predicates below.
        template<typename List>
        struct DuplicateIn {
            template<typename T>
            static constexpr bool holds = MPL::count<T, List> > 1;
        };

        template<template<typename> class IsSecondaryCore>
        struct NotAPeripheral {
            template<typename T>
            static constexpr bool holds = !(isPeripheral<T> || IsSecondaryCore<T>::value);
        };

        struct IsLaunchTimeout {
            template<typename T>
            static constexpr bool holds = requires { T::isLaunchTimeout; };
        };

        struct HasIsrTransform {
            template<typename T>
            static constexpr bool holds
              = ::Kvasir::Startup::Detail::hasIsrTransform<T>;   // Entry.hpp
        };

        struct IsClockSettings {
            template<typename T>
            static constexpr bool holds
              = requires { T::coreClockInit(); } || requires { T::peripheryClockInit(); };
        };

        struct IsChipEntry {
            template<typename T>
            static constexpr bool holds = ::Kvasir::Startup::Detail::isChipEntry<T>;   // Entry.hpp
        };
    }   // namespace Detail

    // ---- each shape rule is its offender list; the rule holds when it is empty --------------
    // Every type listed more than once, once each, in list order.
    template<typename List>
    using DuplicateEntries = MPL::Dedup<MPL::Filter<Detail::DuplicateIn<List>, List>>;

    template<template<typename> class IsSecondaryCore, typename List>
    using NotPeripherals = MPL::Filter<Detail::NotAPeripheral<IsSecondaryCore>, List>;

    namespace Detail {
        template<template<typename> class IsSecondaryCore>
        struct IsSecondary {
            template<typename T>
            static constexpr bool holds = IsSecondaryCore<T>::value;
        };

        template<typename List>
        struct DropFirst {
            using type = brigand::list<>;
        };

        template<typename T, typename... Ts>
        struct DropFirst<brigand::list<T, Ts...>> {
            using type = brigand::list<Ts...>;
        };
    }   // namespace Detail

    // Every SecondaryCore in a list, and every one after the first.
    template<template<typename> class IsSecondaryCore, typename List>
    using Secondaries = MPL::Filter<Detail::IsSecondary<IsSecondaryCore>, List>;

    template<template<typename> class IsSecondaryCore, typename List>
    using ExtraSecondaries = typename Detail::DropFirst<Secondaries<IsSecondaryCore, List>>::type;

    template<typename List>
    using LaunchTimeouts = MPL::Filter<Detail::IsLaunchTimeout, List>;

    // Every entry that rewrites the list's ISRs (an IsrTransform, Entry.hpp), and every one after the first.
    template<typename List>
    using IsrTransformEntries = MPL::Filter<Detail::HasIsrTransform, List>;

    template<typename List>
    using ExtraIsrTransforms = typename Detail::DropFirst<IsrTransformEntries<List>>::type;

    template<typename List>
    using ClockSettingsEntries = MPL::Filter<Detail::IsClockSettings, List>;

    // Every chip entry (chipEntry, Entry.hpp: Startup::ChipEntries) in a list as written: the chip adds them itself.
    template<typename List>
    using ListedChipEntries = MPL::Filter<Detail::IsChipEntry, List>;

    template<typename... Ts>
    struct NoDuplicateEntry<brigand::list<Ts...>>
      : std::bool_constant<Diagnostics::none<DuplicateEntries<brigand::list<Ts...>>>> {};

    // IsSecondaryCore: the chip-agnostic predicate from StartUp.hpp, passed in so a test can
    // substitute its own.
    template<template<typename> class IsSecondaryCore, typename List>
    struct AllArePeripherals;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct AllArePeripherals<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<
          Diagnostics::none<NotPeripherals<IsSecondaryCore, brigand::list<Ts...>>>> {};

    template<template<typename> class IsSecondaryCore, typename List>
    struct AtMostOneSecondary;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct AtMostOneSecondary<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<
          Diagnostics::none<ExtraSecondaries<IsSecondaryCore, brigand::list<Ts...>>>> {};

    template<template<typename> class IsSecondaryCore, typename List>
    struct NoSecondaryIn;

    template<template<typename> class IsSecondaryCore, typename... Ts>
    struct NoSecondaryIn<IsSecondaryCore, brigand::list<Ts...>>
      : std::bool_constant<Diagnostics::none<Secondaries<IsSecondaryCore, brigand::list<Ts...>>>> {
    };

    namespace Detail {
        struct WrongHookShape {
            template<typename T>
            static constexpr bool holds
              = !::Kvasir::Startup::Detail::RuntimeHook::ShapesOk<T>::value;
        };

        // the first hook of T with the wrong shape
        template<typename T, typename Hooks>
        struct FirstWrongHook;

        template<typename T>
        struct FirstWrongHook<T, brigand::list<>> {
            using type = void;
        };

        template<typename T, typename H, typename... Hs>
        struct FirstWrongHook<T, brigand::list<H, Hs...>>
          : std::conditional_t<H::template shapeOk<T>(),
                               FirstWrongHook<T, brigand::list<Hs...>>,
                               std::type_identity<H>> {};
    }   // namespace Detail

    template<typename List>
    struct HookShapesHold;

    // Every entry that declares a hook of the wrong shape.
    template<typename List>
    using WrongHookShapes = MPL::Filter<Detail::WrongHookShape, List>;

    template<typename... Ts>
    struct HookShapesHold<brigand::list<Ts...>>
      : std::bool_constant<Diagnostics::none<WrongHookShapes<brigand::list<Ts...>>>> {};

    // The message, built only when the rule fails (KVASIR_STATIC_ASSERT): the entry, the hook, the signature.
    template<typename Offender>
    struct HookShapeText {
        consteval Kvasir::Diagnostic::Text operator()() const {
            using H =
              typename Detail::FirstWrongHook<Offender,
                                              ::Kvasir::Startup::Detail::RuntimeHook::All>::type;
            Kvasir::Diagnostic::Text t;
            t << "Startup entry ";
            Kvasir::Diagnostic::appendType<Offender>(t);
            t << ": its " << H::name << " must be `" << H::signature
              << "` (it returns another type, is not static, or takes parameters) - Startup would "
                 "never "
                 "call it; on a Startup entry the hook names are reserved";
            return t;
        }
    };

    template<typename List>
    struct NoLaunchTimeoutIn;

    template<typename... Ts>
    struct NoLaunchTimeoutIn<brigand::list<Ts...>>
      : std::bool_constant<Diagnostics::none<LaunchTimeouts<brigand::list<Ts...>>>> {};

    template<typename List>
    struct NoClockSettingsIn;

    template<typename... Ts>
    struct NoClockSettingsIn<brigand::list<Ts...>>
      : std::bool_constant<Diagnostics::none<ClockSettingsEntries<brigand::list<Ts...>>>> {};

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

        // A SecondaryCore's ListedOn<X, 0> claim whose X is listed at or after it (Launch is the
        // SecondaryCore's position): Pair<X, the SecondaryCore>. An X that is not in the primary
        // list at all is dependenciesListed's finding, not this one's, so -1 passes here.
        template<int Launch, typename Secondary, typename Claims, typename... Ts>
        struct LateClaims;

        template<int Launch, typename Secondary, typename... Rs, typename... Ts>
        struct LateClaims<Launch, Secondary, brigand::list<Rs...>, Ts...> {
            using type = brigand::flatten<brigand::list<std::conditional_t<
              (!std::is_void_v<typename ListedOnCore0<Rs>::type>
               && positionOf<typename ListedOnCore0<Rs>::type, Ts...>() >= Launch),
              brigand::list<Diagnostics::Pair<typename ListedOnCore0<Rs>::type, Secondary>>,
              brigand::list<>>...>>;
        };

        template<template<typename> class IsSecondaryCore,
                 template<typename> class SecondaryPeripherals,
                 typename List,
                 typename Seq>
        struct LateDependenciesImpl;

        template<template<typename> class IsSecondaryCore,
                 template<typename> class SecondaryPeripherals,
                 typename... Ts,
                 std::size_t... Is>
        struct LateDependenciesImpl<IsSecondaryCore,
                                    SecondaryPeripherals,
                                    brigand::list<Ts...>,
                                    std::index_sequence<Is...>> {
            using type = brigand::flatten<brigand::list<std::conditional_t<
              IsSecondaryCore<Ts>::value,
              typename LateClaims<static_cast<int>(Is),
                                  Ts,
                                  typename Kvasir::Startup::Detail::ClaimsIn<
                                    typename SecondaryPeripherals<Ts>::type>::type,
                                  Ts...>::type,
              brigand::list<>>...>>;
        };
    }   // namespace Detail

    // ---- enabled without a handler --------------------------------------------------------
    namespace Detail {
        // A peripheral's interrupt-config and periphery-enable actions (Entry.hpp).
        template<typename T>
        using InterruptConfigOf
          = ::Kvasir::Startup::Detail::PhaseActionsOf<T,
                                                      ::Kvasir::Startup::Detail::Phase::Interrupts>;

        template<typename T>
        using PeripheryEnableOf
          = ::Kvasir::Startup::Detail::PhaseActionsOf<T, ::Kvasir::Startup::Detail::Phase::Enable>;

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

    // The interrupt rules as views of the offending indexes above.
    // Traits: the chip's InterruptOffsetTraits (begin, end, disabled); IsrList: Nvic::Isr
    // types, each with an IType::value.
    template<typename Traits, typename IsrList>
    struct IsrIndexesValid
      : std::bool_constant<Diagnostics::none<typename InvalidIsrIndexes<Traits, IsrList>::type>> {};

    // Enabled: std::integral_constant<int, I> per enabled index (EnabledInterruptsIn).
    template<typename Enabled, typename IsrList>
    struct EnabledLinesHandled
      : std::bool_constant<Diagnostics::none<typename UnhandledIndexes<Enabled, IsrList>::type>> {};

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

    // The level rules as views of the offending indexes above. Silent: a std::array<int, N> of
    // the levels that do not count; Prios: an IsrPrioritiesIn result (one core's, or several
    // cores' appended). Unknown levels are IsrLevelsKnown's.
    template<auto Silent, typename Prios>
    struct SingleIsrPriority
      : std::bool_constant<Diagnostics::none<typename MultiLevelIsrIndexes<Silent, Prios>::type>> {
    };

    // Served: a std::array<int, N> of the levels the entry has a context for.
    template<auto Served, auto Silent, typename Prios>
    struct IsrPrioritiesServed
      : std::bool_constant<
          Diagnostics::none<typename UnservedIsrIndexes<Served, Silent, Prios>::type>> {};

    // Every enabled index at a level the core layer could read.
    template<typename Prios>
    struct IsrLevelsKnown
      : std::bool_constant<Diagnostics::none<typename UnknownIsrLevelIndexes<Prios>::type>> {};

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

    // Pair<X, SecondaryCore> per core 0 peripheral X a SecondaryCore's list needs (ListedOn<X,
    // 0>) that is listed at or after that SecondaryCore.
    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename List>
    using LateDependencies = typename Detail::LateDependenciesImpl<
      IsSecondaryCore,
      SecondaryPeripherals,
      List,
      std::make_index_sequence<brigand::size<List>::value>>::type;

    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename List>
    struct DependenciesPrecedeLaunch
      : std::bool_constant<
          Diagnostics::none<LateDependencies<IsSecondaryCore, SecondaryPeripherals, List>>> {};

    // ---- the boot report (Runtime/BootReport.hpp, Runtime/BootGuard.hpp) --------------------------
    template<typename T>
    constexpr bool isBootReport = requires { requires T::isBootReport; };

    template<typename T>
    constexpr bool isBootReportInMain = requires { requires T::bootReportInMain; };

    template<typename T>
    constexpr bool isBootGuard = requires { requires T::isBootGuard; };

    template<typename T>
    constexpr bool isHealthService = requires { requires T::isHealthService; };

    template<typename T>
    constexpr bool isWatchdog = requires { requires T::isWatchdog; };

    template<typename T>
    constexpr bool isGatedWatchdog = requires { requires T::isGatedWatchdog; };

    namespace Detail {
        template<bool Reported, typename Out, typename... Ts>
        struct GuardsAfterReportImpl {
            using type = Out;
        };

        template<bool Reported, typename... Os, typename T, typename... Ts>
        struct GuardsAfterReportImpl<Reported, brigand::list<Os...>, T, Ts...>
          : GuardsAfterReportImpl<Reported || isBootReport<T>,
                                  std::conditional_t<Reported && isBootGuard<T>,
                                                     brigand::list<Os..., T>,
                                                     brigand::list<Os...>>,
                                  Ts...> {};

        template<typename List>
        struct GuardsAfterReportT;

        template<typename... Ts>
        struct GuardsAfterReportT<brigand::list<Ts...>>
          : GuardsAfterReportImpl<false, brigand::list<>, Ts...> {};

        // Pending: the services seen since the last report; a report moves them to Out.
        template<typename Pending, typename Out, typename... Ts>
        struct ServicesBeforeReportImpl {
            using type = Out;
        };

        template<typename... Ps, typename... Os, typename T, typename... Ts>
        struct ServicesBeforeReportImpl<brigand::list<Ps...>, brigand::list<Os...>, T, Ts...>
          : ServicesBeforeReportImpl<std::conditional_t<isBootReport<T>,
                                                        brigand::list<>,
                                                        std::conditional_t<isHealthService<T>,
                                                                           brigand::list<Ps..., T>,
                                                                           brigand::list<Ps...>>>,
                                     std::conditional_t<isBootReport<T>,
                                                        brigand::list<Os..., Ps...>,
                                                        brigand::list<Os...>>,
                                     Ts...> {};

        template<bool Serviced, typename Out, typename... Ts>
        struct WatchdogsAfterServiceImpl {
            using type = Out;
        };

        template<bool Serviced, typename... Os, typename T, typename... Ts>
        struct WatchdogsAfterServiceImpl<Serviced, brigand::list<Os...>, T, Ts...>
          : WatchdogsAfterServiceImpl<
              Serviced || isHealthService<T>,
              std::conditional_t<Serviced && isWatchdog<T> && !isGatedWatchdog<T>,
                                 brigand::list<Os..., T>,
                                 brigand::list<Os...>>,
              Ts...> {};

        template<typename List>
        struct WatchdogsAfterServiceT;

        template<typename... Ts>
        struct WatchdogsAfterServiceT<brigand::list<Ts...>>
          : WatchdogsAfterServiceImpl<false, brigand::list<>, Ts...> {};

        template<typename List>
        struct ServicesBeforeReportT;

        template<typename... Ts>
        struct ServicesBeforeReportT<brigand::list<Ts...>>
          : ServicesBeforeReportImpl<brigand::list<>, brigand::list<>, Ts...> {};

        template<typename Backend, typename List>
        struct UnreportedBackendsT;

        template<typename Backend, typename... Ts>
        struct UnreportedBackendsT<Backend, brigand::list<Ts...>> {
            using type
              = std::conditional_t<(std::is_same_v<Backend, Ts> || ...)
                                     && !((isBootReport<Ts> || isBootReportInMain<Ts>) || ...),
                                   brigand::list<Backend>,
                                   brigand::list<>>;
        };
    }   // namespace Detail

    // Every BootGuard::Guard listed after a Boot::Report, in list order.
    template<typename List>
    using GuardsAfterReport = typename Detail::GuardsAfterReportT<List>::type;

    template<typename List>
    struct GuardBeforeReport : std::bool_constant<Diagnostics::none<GuardsAfterReport<List>>> {};

    // Every Health::Service<> listed before a Boot::Report, in list order.
    template<typename List>
    using ServicesBeforeReport = typename Detail::ServicesBeforeReportT<List>::type;

    template<typename List>
    struct HealthServiceAfterReport
      : std::bool_constant<Diagnostics::none<ServicesBeforeReport<List>>> {};

    // Every chip watchdog listed after a Health::Service<>, in list order.
    template<typename List>
    using WatchdogsAfterService = typename Detail::WatchdogsAfterServiceT<List>::type;

    template<typename List>
    struct WatchdogBeforeHealthService
      : std::bool_constant<Diagnostics::none<WatchdogsAfterService<List>>> {};

    // The user log backend, when the list names it and no entry is the boot report or says main reports by hand.
    template<typename Backend, typename List>
    using UnreportedBackends = typename Detail::UnreportedBackendsT<Backend, List>::type;

    template<typename Backend, typename List>
    struct BootReportListed
      : std::bool_constant<Diagnostics::none<UnreportedBackends<Backend, List>>> {};

}}}   // namespace Kvasir::Startup::ListRules
