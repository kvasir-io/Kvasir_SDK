#pragma once

#include "kvasir/Common/Core.hpp"
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Utility.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/StartUp/IsrProfiler.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/StartUp/SharedIsr.hpp"
#include "kvasir/Util/ImageDescriptor.hpp"
#include "kvasir/Util/Panic.hpp"
#include "kvasir/Util/attributes.hpp"
#include "kvasir/Util/ubsan.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

// declaring and calling main is ill-formed in ISO C++ but intended here; gcc diagnoses it under -Wpedantic
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern "C" {
extern int main();
}
#pragma GCC diagnostic pop

#ifdef __arm__
extern "C" {
// The value every -fstack-protector function copies onto its stack on entry and compares on exit.
// In .noInit: neither initMemory() nor a reset touches it, so a protected function that is on the
// stack while .data/.bss are set up (the clock init) still finds the value it started with. Seeded
// once per boot by core 0 (Startup::Detail::seedStackGuard); volatile for the reason
// Fault::lastFault is (LTO narrows an uninitialised object otherwise). A wild write that hits it
// shows up as canary failures in unrelated functions: peek it and compare with its boot value.
[[gnu::used, gnu::section(".noInit")]] inline std::uint32_t volatile __stack_chk_guard;
}
#endif

namespace Kvasir { namespace Startup {
    namespace Detail {
        using namespace MPL;
        namespace br = brigand;

        template<typename T>
        struct Listify {
            static_assert(AlwaysFalse<T>::value,
                          "implausible type");
        };

        template<typename T, typename U>
        struct Listify<Register::Action<T, U>> : br::list<Register::Action<T, U>> {};

        template<typename... Ts>
        struct Listify<br::list<Ts...>> : br::list<Ts...> {};

        template<typename T, typename = void>
        struct GetEarlyInit : br::list<> {};

        template<typename T>
        struct GetEarlyInit<T, VoidT<decltype(T::earlyInit)>>
          : Listify<RemoveCVT<decltype(T::earlyInit)>> {};

        template<typename T, typename = void>
        struct GetPowerClockInit : br::list<> {};

        template<typename T>
        struct GetPowerClockInit<T, VoidT<decltype(T::powerClockEnable)>>
          : Listify<RemoveCVT<decltype(T::powerClockEnable)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPinInit : br::list<> {};

        template<typename T>
        struct GetPinInit<T, void, VoidT<decltype(T::initStepPinConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepPinConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPeripheryInit : br::list<> {};

        template<typename T>
        struct GetPeripheryInit<T, void, VoidT<decltype(T::initStepPeripheryConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepPeripheryConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetInterruptInit : br::list<> {};

        template<typename T>
        struct GetInterruptInit<T, void, VoidT<decltype(T::initStepInterruptConfig)>>
          : Listify<RemoveCVT<decltype(T::initStepInterruptConfig)>> {};

        template<typename T, typename = void, typename = void>
        struct GetPeripheryEnableInit : br::list<> {};

        template<typename T>
        struct GetPeripheryEnableInit<T, void, VoidT<decltype(T::initStepPeripheryEnable)>>
          : Listify<RemoveCVT<decltype(T::initStepPeripheryEnable)>> {};

        template<int I>
        struct IsIsrByIndex {
            template<typename T>
            struct Apply : Bool<(T::IType::value == I)>::type {};
        };

        template<int I, typename TList, typename TModList>
        struct CompileIsrPointerList;

        template<int I, typename... Ts, typename TModList>
        struct CompileIsrPointerList<I, br::list<Ts...>, TModList>
          : CompileIsrPointerList<
              I + 1,
              br::list<Ts...,
                       GetT<TModList, Template<IsIsrByIndex<I>::template Apply>, Nvic::UnusedIsr>>,
              TModList> {};

        template<typename... Ts, typename TModList>
        struct CompileIsrPointerList<Nvic::InterruptOffsetTraits<void>::end,
                                     br::list<Ts...>,
                                     TModList> : br::list<Ts...> {};

        // predicate returning result of left < right for RegisterOptions
        template<typename TLeft, typename TRight>
        struct ListLengthLess : Bool<(SizeT<TLeft>::value < SizeT<TRight>::value)> {};

        using ListLengthLessP = Template<ListLengthLess>;

        template<typename TOut, typename TList>
        struct Merge;

        template<typename... Os, typename... Ts>
        struct Merge<br::list<Os...>, br::list<br::list<>, Ts...>>
          : Merge<   // if next is empty list remove it and continue
              br::list<Os...>,
              br::list<Ts...>> {};

        template<typename... Os, typename... Ts>
        struct Merge<br::list<Os...>, br::list<Ts...>>
          : Merge<br::list<Os..., br::flatten<br::list<AtT<Ts, Int<0>>...>>>,
                  br::list<RemoveT<Ts, Int<0>, Int<1>>...>> {};

        template<typename... Os>
        struct Merge<br::list<Os...>, br::list<>> : br::list<Os...> {};

        template<typename T, typename = void, typename = void>
        struct ExtractIsr : br::list<> {};

        // Two peripherals on one vector: CompileIsrPointerList takes the first match for
        // an index, so the second ISR would silently never run. Two DmaBase instances
        // without an interruptInstance each are the way to get here.
        template<typename I,
                 typename... Is>
        constexpr int isrsOnIndex(br::list<Is...>) {
            return (0 + ... + int{std::is_same_v<typename I::IType, typename Is::IType>});
        }

        template<typename List>
        struct UniqueIsrIndexes;

        template<typename... Is>
        struct UniqueIsrIndexes<br::list<Is...>>
          : Bool<((isrsOnIndex<Is>(br::list<Is...>{}) == 1) && ...)> {};

        template<typename T, typename U>
        struct ExtractIsr<T, U, VoidT<typename T::Isr>> : T::Isr {};

        template<typename T>
        struct ExtractIsr<T, void, VoidT<decltype(T::isr)>>
          : std::remove_const_t<decltype(T::isr)> {};

        template<typename List>
        struct IsrsOf;

        template<typename... Ts>
        struct IsrsOf<br::list<Ts...>> {
            using type = br::flatten<br::list<typename ExtractIsr<Ts>::type...>>;
        };

        // A vector installed in both cores' tables. Most interrupt lines are chip-wide and
        // reach both NVICs: enable one on both cores and the ISR runs on both cores at
        // once, each clearing the flag the other was about to look at. The lines that are
        // legitimately per core (SIO FIFO and doorbells, IO_BANK0, the core exceptions) are
        // the chip's `InterruptOffsetTraits::perCore`; a chip that lists none has none.
        template<typename Traits = Nvic::InterruptOffsetTraits<void>>
        constexpr bool isPerCoreVector(int index) {
            if(index < 0) { return true; }
            if constexpr(requires { Traits::perCore; }) {
                for(auto const i : Traits::perCore) {
                    if(i == index) { return true; }
                }
            }
            return false;
        }

        template<typename I,
                 typename... Is>
        constexpr bool vectorAlsoIn(br::list<Is...>) {
            return (false || ... || std::is_same_v<typename I::IType, typename Is::IType>);
        }

        template<typename A, typename B>
        struct VectorsDisjoint;

        template<typename... As, typename B>
        struct VectorsDisjoint<br::list<As...>, B>
          : Bool<((isPerCoreVector<>(As::IType::value) || !vectorAlsoIn<As>(B{})) && ...)> {};

    }   // namespace Detail

    // The vector table for a given initial stack pointer and reset entry, followed by every
    // ISR the peripherals claim. The boot core uses the linker's stack and ResetISR; a
    // SecondaryCore its own stack and entry trampoline.
    template<Nvic::IsrFunctionPointer StackEnd, Nvic::IsrFunctionPointer Reset, typename... Ts>
    struct GetIsrPointersFor
      : Detail::CompileIsrPointerList<
          Nvic::InterruptOffsetTraits<void>::begin,
          brigand::list<Nvic::Isr<StackEnd, Nvic::Index<0>>, Nvic::Isr<Reset, Nvic::Index<0>>>,
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>> {
        // named constants: a short "due to requirement" line (the reports name the index)
        static constexpr bool uniqueIsrIndexes = Detail::UniqueIsrIndexes<
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value;
        static constexpr bool isrIndexesValid = ListRules::IsrIndexesValid<
          Nvic::InterruptOffsetTraits<void>,
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value;
        static_assert(
          uniqueIsrIndexes,
          "two peripherals in one Startup list claim the same interrupt vector: only the "
          "first would ever run (two DmaBase instances need an interruptInstance each)");
        static_assert(
          isrIndexesValid,
          "a peripheral installs an Isr on an index the chip's vector table does not have "
          "(outside InterruptOffsetTraits::begin..end, or a disabled index): it would be "
          "silently absent from the table");
    };

    template<Nvic::IsrFunctionPointer StackEnd, Nvic::IsrFunctionPointer Reset, typename... Ts>
    using GetIsrPointersForT = typename GetIsrPointersFor<StackEnd, Reset, Ts...>::type;

    // The boot core's table: the linker's stack and the Startup's own reset entry.
    template<Nvic::IsrFunctionPointer Reset>
    struct BootIsrPointers {
        template<typename... Ts>
        using type = GetIsrPointersForT<std::addressof(_LINKER_stack_end_), Reset, Ts...>;
    };

    namespace Detail {
        // A template applied to the elements of a brigand::list: Startup and SecondaryCore work on
        // their peripherals plus the generated shared-vector dispatchers (SharedIsr.hpp).
        template<template<typename...> class F, typename List>
        struct ExpandList;

        template<template<typename...> class F, typename... Ts>
        struct ExpandList<F, brigand::list<Ts...>> {
            using type = F<Ts...>;
        };

        template<template<typename...> class F, typename List>
        using ExpandListT = typename ExpandList<F, List>::type;

        template<Nvic::IsrFunctionPointer StackEnd, Nvic::IsrFunctionPointer Reset, typename List>
        struct IsrPointersForList;

        template<Nvic::IsrFunctionPointer StackEnd, Nvic::IsrFunctionPointer Reset, typename... Ts>
        struct IsrPointersForList<StackEnd, Reset, brigand::list<Ts...>> {
            using type = GetIsrPointersForT<StackEnd, Reset, Ts...>;
        };
    }   // namespace Detail

    // Defined in SecondaryCore.hpp, included at the end of this file. Declared here so the
    // guards in Startup can recognise one in a peripheral list.
    template<void (*Main)(), typename... Peripherals>
    struct SecondaryCore;

    namespace Detail {
        template<typename T>
        struct IsSecondaryCore : std::false_type {};

        template<void (*Main)(), typename... Ps>
        struct IsSecondaryCore<SecondaryCore<Main, Ps...>> : std::true_type {};

        template<typename T>
        struct SecondaryPeripherals {
            using type = brigand::list<>;
        };

        template<void (*Main)(), typename... Ps>
        struct SecondaryPeripherals<SecondaryCore<Main, Ps...>> {
            using type = brigand::list<Ps...>;
        };

        template<typename List, typename P>
        struct Contains;

        template<typename... Us, typename P>
        struct Contains<brigand::list<Us...>, P> : Bool<(std::is_same_v<Us, P> || ...)> {};

        template<typename Ps, typename All>
        struct Disjoint;

        template<typename... Ps, typename All>
        struct Disjoint<brigand::list<Ps...>, All> : Bool<(!Contains<All, Ps>::value && ...)> {};

        // A peripheral in a SecondaryCore's list must not also be in the primary list: its
        // interrupt would be enabled on both cores, and its init steps run twice.
        template<typename... Ts>
        struct NoPeripheralOnBothCores
          : Bool<(
              (!IsSecondaryCore<Ts>::value
               || Disjoint<typename SecondaryPeripherals<Ts>::type, brigand::list<Ts...>>::value)
              && ...)> {};

        // The per-core peripheral lists the resource check runs over, core 0's first and
        // then one per SecondaryCore, in list order. The list index is the core number,
        // which is what rule 5 (startupCore) compares against - so entries that are not a
        // SecondaryCore contribute no list at all, rather than an empty one that would
        // push core 1's list to some other index.
        template<typename Out, typename... Ts>
        struct CoreListsImpl;

        template<typename... Os>
        struct CoreListsImpl<brigand::list<Os...>> {
            using type = brigand::list<Os...>;
        };

        template<typename... Os, typename T, typename... Ts>
        struct CoreListsImpl<brigand::list<Os...>, T, Ts...>
          : CoreListsImpl<
              std::conditional_t<IsSecondaryCore<T>::value,
                                 brigand::list<Os..., typename SecondaryPeripherals<T>::type>,
                                 brigand::list<Os...>>,
              Ts...> {};

        template<typename Primary, typename... Ts>
        using CoreLists = typename CoreListsImpl<brigand::list<Primary>, Ts...>::type;

        template<typename Lists>
        struct ResourceCheckOver;

        template<typename... Ls>
        struct ResourceCheckOver<brigand::list<Ls...>> {
            using type = ResourceCheck<Ls...>;
        };

        // The list rules' messages with their first offender (ListRules.hpp), built only when one fails.
        struct DuplicateEntryRule {
            static constexpr std::string_view message
              = "a peripheral is listed twice in one Startup list (its init steps would run twice)";
        };

        struct NotAPeripheralRule {
            static constexpr std::string_view message
              = "a type in the Startup list is not a peripheral (no init step, Isr, runtime hook, "
                "Provides/Claims "
                "or SecondaryCore): a driver's Config listed instead of the driver, or a typo'd "
                "alias - it would "
                "contribute nothing";
        };

        struct LaunchTimeoutRule {
            static constexpr std::string_view message
              = "a LaunchTimeout in the primary Startup list is ignored: it belongs in the "
                "SecondaryCore's list "
                "whose launch it bounds";
        };

        struct ClockSettingsRule {
            static constexpr std::string_view message
              = "a ClockSettings among the peripherals: its clock init never runs there (Startup's "
                "first argument "
                "is the one that does)";
        };

        template<typename Rule, typename Offender>
        struct ListRuleText {
            consteval Kvasir::Diagnostic::Text operator()() const {
                Kvasir::Diagnostic::Text t;
                t << Rule::message << ": ";
                Kvasir::Diagnostic::appendType<Offender>(t);
                return t;
            }
        };

        // The interrupt rules' reports (Diagnostics::Report, Resources.hpp): each names the
        // index in its instantiation. `Where` is the list's marker type, Startup or a
        // SecondaryCore, so the trail also says which core.
        struct VectorTwice {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "two peripherals in one Startup list claim the same interrupt vector: only "
                "the first would ever run (two DmaBase instances need an interruptInstance "
                "each)";
        };

        struct VectorOnBothCores {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "an interrupt is installed in both cores' vector tables: the ISR would run "
                "on both cores at once (only the SIO, IO_BANK0 and core-exception vectors "
                "are per core): list the peripheral on one core";
        };

        struct VectorOutOfTable {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "a peripheral installs an Isr on an index the chip's vector table does not "
                "have (outside InterruptOffsetTraits::begin..end, or a disabled index): it "
                "would be silently absent from the table";
        };

        struct EnabledUnhandled {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "an init step enables an interrupt line for which nothing in this core's "
                "list installs an Isr: the line would fire into the unhandled-interrupt "
                "handler at its first event (a driver whose Isr alias was dropped, or an "
                "interruptEnable copied from another driver)";
        };

        // An entry's ISR-context contract (ListRules.hpp: IsrContractHolds) against the enabled
        // interrupts, reported per (entry, index).
        struct IsrLevelUnserved {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "an interrupt is enabled at a priority level this entry has no ISR context "
                "for: an ISR at that level could preempt another ISR's log record on a ring "
                "it shares. Give the level its own context (uc_log: add it to the "
                "IsrPolicy::PerLevel Levels<...>, preferred), declare it silent if no ISR at "
                "that level ever logs (SilentLevels<...>), or use the entry's masked-record "
                "mode (IsrPolicy::MaskedRecord)";
        };

        struct IsrLevelsDiffer {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "enabled interrupts that may log use more than one priority level, and this "
                "entry keeps one ISR context for all of them: an ISR at the higher level "
                "preempts one at the lower mid-record. Configure the entry per level (uc_log: "
                "IsrPolicy::PerLevel, preferred), declare the levels whose ISRs never log as "
                "silent (SilentLevels<...>, SysTick's level 0 typically), or use its "
                "masked-record mode (IsrPolicy::MaskedRecord)";
        };

        struct IsrLevelUnknown {
            static constexpr auto             kind = Diagnostics::NumberKind::interrupt;
            static constexpr std::string_view message
              = "an init step writes this interrupt's priority in a form the check cannot "
                "read (a run-time value, a toggle, or a literal over part of the priority "
                "bits), so its level is unknown and this entry's ISR contexts cannot be shown "
                "to cover it. Set the priority with a literal (Nvic::makeSetPriority), or use "
                "the entry's masked-record mode (uc_log: IsrPolicy::MaskedRecord)";
        };

        template<typename Rule, typename Where, typename Indexes>
        struct ReportIndexes;

        template<typename Rule, typename Where, typename... Is>
        struct ReportIndexes<Rule, Where, brigand::list<Is...>> {
            static constexpr bool value
              = ((sizeof(Diagnostics::Report<Rule, Where, Is>) > 0) && ...);
        };

        // The indexes core 0 and a SecondaryCore both install, chip-wide ones only.
        template<typename Primary, typename Secondary>
        struct SharedVectorIndexes;

        template<typename... As, typename Secondary>
        struct SharedVectorIndexes<brigand::list<As...>, Secondary> {
            using type = brigand::flatten<brigand::list<ListRules::Detail::IndexIf<
              (!isPerCoreVector<>(As::IType::value) && vectorAlsoIn<As>(Secondary{})),
              As::IType::value>...>>;
        };

        // The list-level interrupt checks with their reports, for one core's list.
        template<typename Where, typename List>
        struct InterruptReports {
            using Isrs    = typename IsrsOf<List>::type;
            using Enabled = typename ListRules::EnabledInterruptsIn<List>::type;

            static constexpr bool value
              = ReportIndexes<VectorTwice,
                              Where,
                              typename ListRules::DuplicateIsrIndexes<Isrs>::type>::value
             && ReportIndexes<
                  VectorOutOfTable,
                  Where,
                  typename ListRules::InvalidIsrIndexes<Nvic::InterruptOffsetTraits<void>,
                                                        Isrs>::type>::value
             && ReportIndexes<EnabledUnhandled,
                              Where,
                              typename ListRules::UnhandledIndexes<Enabled, Isrs>::type>::value;
        };

        // The entries an ISR-context contract is read from: every core's list, and the user
        // log backend even when no list names it, since listing it is only a convention.
        template<typename Backend>
        constexpr bool isCompleteType = requires { sizeof(Backend); };

        template<typename CoreLists, typename Backend>
        struct IsrContractEntries;

        template<typename... Ls, typename Backend>
        struct IsrContractEntries<brigand::list<Ls...>, Backend> {
            using Listed = brigand::append<Ls...>;
            using type
              = std::conditional_t<isCompleteType<Backend> && !Contains<Listed, Backend>::value,
                                   brigand::append<Listed, brigand::list<Backend>>,
                                   Listed>;
        };

        // The reports of one entry's contract on one group of interrupts
        // (ListRules::IsrPriorityGroupsFor), `T` in the trail: entries without one pass.
        template<typename Prios, typename T>
        struct IsrLevelReportsFor : Bool<true> {};

        template<typename Prios, typename T>
            requires(ListRules::declaresIsrLevels<T> && !ListRules::declaresSingleIsrLevel<T>)
        struct IsrLevelReportsFor<Prios, T>
          : Bool<ReportIndexes<IsrLevelUnknown,
                               T,
                               typename ListRules::UnknownIsrLevelIndexes<Prios>::type>::value
                 && ReportIndexes<
                   IsrLevelUnserved,
                   T,
                   typename ListRules::UnservedIsrIndexes<T::isrPriorityLevels,
                                                          ListRules::silentIsrLevelsOf<T>(),
                                                          Prios>::type>::value> {};

        template<typename Prios, typename T>
            requires(ListRules::declaresSingleIsrLevel<T> && !ListRules::declaresIsrLevels<T>)
        struct IsrLevelReportsFor<Prios, T>
          : Bool<ReportIndexes<IsrLevelUnknown,
                               T,
                               typename ListRules::UnknownIsrLevelIndexes<Prios>::type>::value
                 && ReportIndexes<
                   IsrLevelsDiffer,
                   T,
                   typename ListRules::MultiLevelIsrIndexes<ListRules::silentIsrLevelsOf<T>(),
                                                            Prios>::type>::value> {};

        template<typename T, typename Groups>
        struct IsrContractReportsOn;

        template<typename T, typename... Gs>
        struct IsrContractReportsOn<T, brigand::list<Gs...>>
          : Bool<(IsrLevelReportsFor<Gs, T>::value && ...)> {};

        template<typename Entries, typename CoreLists>
        struct IsrContractReports;

        template<typename... Ts, typename CoreLists>
        struct IsrContractReports<brigand::list<Ts...>, CoreLists>
          : Bool<(IsrContractReportsOn<
                    Ts,
                    typename ListRules::IsrPriorityGroupsFor<Ts, CoreLists>::type>::value
                  && ...)> {};

        // The same rule as a plain predicate, for the assert that carries the message.
        template<typename Entries, typename CoreLists>
        struct IsrContractsHold;

        template<typename... Ts, typename CoreLists>
        struct IsrContractsHold<brigand::list<Ts...>, CoreLists>
          : Bool<(ListRules::IsrContractHolds<Ts, CoreLists>::value && ...)> {};

        template<typename Entries>
        struct NoContradictingIsrContract;

        template<typename... Ts>
        struct NoContradictingIsrContract<brigand::list<Ts...>>
          : Bool<(!(ListRules::declaresIsrLevels<Ts> && ListRules::declaresSingleIsrLevel<Ts>)
                  && ...)> {};

        // Core 0's vectors against every SecondaryCore's.
        template<typename Primary, typename... Ts>
        struct NoVectorOnBothCores
          : Bool<((!IsSecondaryCore<Ts>::value
                   || VectorsDisjoint<
                     typename IsrsOf<Primary>::type,
                     typename IsrsOf<typename SecondaryPeripherals<Ts>::type>::type>::value)
                  && ...)> {};

    }   // namespace Detail

    template<typename... Ts>
    struct GetEarlyInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetEarlyInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetEarlyInitT = typename GetEarlyInit<Ts...>::type;

    template<typename... Ts>
    struct GetPowerClockInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPowerClockInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPowerClockInitT = typename GetPowerClockInit<Ts...>::type;

    template<typename... Ts>
    struct GetPinInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPinInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPinInitT = typename GetPinInit<Ts...>::type;

    template<typename... Ts>
    struct GetPeripheryInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPeripheryInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPeripheryInitT = typename GetPeripheryInit<Ts...>::type;

    template<typename... Ts>
    struct GetInterruptInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetInterruptInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetInterruptInitT = typename GetInterruptInit<Ts...>::type;

    template<typename... Ts>
    struct GetPeripheryEnableInit {
        // make list of lists of actions corresponding to each sequence for each module
        using FlattenedSequencePieces
          = brigand::list<brigand::flatten<typename Detail::GetPeripheryEnableInit<Ts>::type>...>;
        using type = brigand::flatten<FlattenedSequencePieces>;
    };

    template<typename... Ts>
    using GetPeripheryEnableInitT = typename GetPeripheryEnableInit<Ts...>::type;

    template<typename T>
    struct NvicVectorTable;

    template<typename... Ts>
    struct NvicVectorTable<brigand::list<Ts...>> {
        std::array<Kvasir::Nvic::IsrFunctionPointer, sizeof...(Ts)> data{Ts::value...};
    };

    // A vector table with the alignment VTOR demands: the next power of two at or above the
    // table's size. Used for a SecondaryCore's table, which lives at an arbitrary address;
    // the boot core's sits at the start of flash and is aligned by construction.
    namespace Detail {
        constexpr std::size_t vectorTableAlignment(std::size_t entries) {
            std::size_t alignment = 128;
            while(alignment < entries * sizeof(Kvasir::Nvic::IsrFunctionPointer)) {
                alignment *= 2;
            }
            return alignment;
        }
    }   // namespace Detail

    template<typename T>
    struct has_runtimeInit {
        template<typename U>
        static constexpr std::false_type test(...) noexcept {
            return {};
        }

        template<typename U>
        static constexpr auto test(U*) noexcept ->
          typename std::is_same<void,
                                decltype(U::runtimeInit())>::type {
            return {};
        }

        static constexpr bool value = test<T>(nullptr);
    };

    template<typename T>
    struct has_preEnableRuntimeInit {
        template<typename U>
        static constexpr std::false_type test(...) noexcept {
            return {};
        }

        template<typename U>
        static constexpr auto test(U*) noexcept ->
          typename std::is_same<void,
                                decltype(U::preEnableRuntimeInit())>::type {
            return {};
        }

        static constexpr bool value = test<T>(nullptr);
    };

    template<typename T>
    void callPreEnableRuntimeInit() {
        if constexpr(has_preEnableRuntimeInit<T>::value) { T::preEnableRuntimeInit(); }
    }

    template<typename... Ts>
    void callPreEnableRuntimeInits() {
        (callPreEnableRuntimeInit<Ts>(), ...);
    }

    template<typename T>
    void callRuntimeInit() {
        if constexpr(has_runtimeInit<T>::value) { T::runtimeInit(); }
    }

    template<typename... Ts>
    void callRuntimeInits() {
        (callRuntimeInit<Ts>(), ...);
    }

    namespace Detail {
        // runtimeInits run in list order and a SecondaryCore's launches the other core, so
        // anything with a runtimeInit after it would run concurrently with that core's main.
        template<typename... Ts>
        constexpr bool noRuntimeInitAfterSecondaryCore() {
            constexpr std::array<bool, sizeof...(Ts)> secondary{IsSecondaryCore<Ts>::value...};
            constexpr std::array<bool, sizeof...(Ts)> runtime{has_runtimeInit<Ts>::value...};
            bool                                      launched = false;
            for(std::size_t i = 0; i < sizeof...(Ts); ++i) {
                if(launched && runtime[i]) { return false; }
                if(secondary[i]) { launched = true; }
            }
            return true;
        }
    }   // namespace Detail

    [[gnu::always_inline]] inline void initMemory() {
        auto data_start = std::addressof(_LINKER_data_start_);
        asm("" : "+l"(data_start)::);

        auto data_start_flash = std::addressof(_LINKER_data_start_flash_);
        asm("" : "+l"(data_start_flash)::);

        auto data_size = reinterpret_cast<std::size_t>(std::addressof(_LINKER_data_size_));
        asm("" : "+l"(data_size)::);

        std::memcpy(data_start, data_start_flash, data_size);

        auto bss_start = std::addressof(_LINKER_bss_start_);
        asm("" : "+l"(bss_start)::);

        auto bss_size = reinterpret_cast<std::size_t>(std::addressof(_LINKER_bss_size_));
        asm("" : "+l"(bss_size)::);

        std::memset(bss_start, 0, bss_size);
    }

    [[gnu::always_inline]] inline void callGlobalConstructors() {
        auto init_begin = std::addressof(_LINKER_init_array_start_);
        asm("" : "+l"(init_begin)::);

        auto init_end = std::addressof(_LINKER_init_array_end_);
        asm("" : "+l"(init_end)::);

        while(init_begin < init_end) {
            (*init_begin)();
            ++init_begin;
        }
    }

    namespace Detail {

        // Once per boot on core 0, while no protected function that will return is on the stack
        // (ResetISR never returns and this inlines into it). Core 1 shares the guard and never
        // seeds. The low byte is zero: a terminator, so an unbounded string copy cannot write the
        // canary back.
        [[gnu::always_inline,
          gnu::no_stack_protector]] inline void
        seedStackGuard() {
#ifdef __arm__
            __stack_chk_guard = StackGuardEntropy<Kvasir::Tag::User>{}() & 0xFFFFFF00U;
#endif
        }

        struct NoOpStartupHook {
            [[gnu::always_inline]] void operator()() const noexcept {}
        };

        // Shared ResetISR body. Hook is called immediately after FirstInitStep,
        // before any ISR fires — used by StartupWithProfiling to enable the
        // DWT cycle counter; NoOpStartupHook for plain Startup.
        template<typename ClockSettings, typename... Peripherals>
        struct StartupImpl;

        template<typename ClockSettings, typename List>
        struct StartupImplOf;

        template<typename ClockSettings, typename... Ts>
        struct StartupImplOf<ClockSettings, brigand::list<Ts...>> {
            using type = StartupImpl<ClockSettings, Ts...>;
        };

        // An image does not always start from a clean reset: a RAM image the debugger starts, or an
        // application a bootloader jumps into, inherits whatever ran before (e.g. the RP2040 boot ROM's
        // USB interrupt, enabled and pending). Every NVIC enable and pending bit is cleared before
        // anything else; Startup enables what the list claims. Armv6-M: one NVIC_ICER / NVIC_ICPR (DDI0419E B3.4, 0xE000E180 / 0xE000E280); Armv8-M:
        // NVIC_ICERn / NVIC_ICPRn, n = 0..15, "always implemented" (DDI0553B.y D1.2.183/184).
        inline void disableAllInterrupts() {
#ifdef __arm__
    #if defined(__ARM_ARCH_6M__)
            constexpr std::uint32_t Words = 1;
    #else
            constexpr std::uint32_t Words = 16;
    #endif
            for(std::uint32_t n = 0; n != Words; ++n) {
                *reinterpret_cast<std::uint32_t volatile*>(0xE000E180U + 4U * n) = 0xFFFFFFFFU;
                *reinterpret_cast<std::uint32_t volatile*>(0xE000E280U + 4U * n) = 0xFFFFFFFFU;
            }
            asm volatile("dsb\n isb" ::: "memory");
#endif
        }

        // Gives the reset entry - a member function of the firmware's Startup type - the name
        // ResetISR in the image, for `--entry=ResetISR` (compiler_common.cmake), debuggers and
        // backtraces. Only directives, no instruction. Two Startups in one image are a duplicate
        // symbol at link time.
        template<Nvic::IsrFunctionPointer Reset>
        [[gnu::always_inline]] inline void nameResetEntry() {
#ifdef __arm__
            asm(".globl ResetISR\n\t.thumb_set ResetISR, %c0" ::"i"(Reset));
#endif
        }

        template<typename ClockSettings, typename... Peripherals>
        struct StartupImpl {
            template<typename Hook = NoOpStartupHook>
            [[noreturn,
              gnu::always_inline]] static void
            ResetISR() {
                FirstInitStep<Kvasir::Tag::User>{}();
                Detail::disableAllInterrupts();
                Hook{}();

                Kvasir::Register::apply(GetEarlyInitT<Peripherals...>{});

                ClockSettings::coreClockInit();

                initMemory();
                ExtraMemoryInit<Kvasir::Tag::User>{}();   // the chip's extra RAM sections, if any

                // after the clocks (a chip's entropy may need them), before any constructor
                Detail::seedStackGuard();

                callGlobalConstructors();

                ClockSettings::peripheryClockInit();

                Kvasir::Register::apply(GetPowerClockInitT<Peripherals...>{});
                Kvasir::Register::apply(GetPinInitT<Peripherals...>{});
                Kvasir::Register::apply(GetPeripheryInitT<Peripherals...>{});
                Kvasir::Register::apply(GetInterruptInitT<Peripherals...>{});
                callPreEnableRuntimeInits<Peripherals...>();
                Kvasir::Nvic::enable_all();
                Kvasir::Register::apply(GetPeripheryEnableInitT<Peripherals...>{});
                callRuntimeInits<Peripherals...>();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
                main();
#pragma GCC diagnostic pop
                assert(false);
            }
        };

    }   // namespace Detail

    // ISR pointer builder with profiling transformation applied to the
    // peripheral ISR list before it reaches CompileIsrPointerList.
    // Stack-end and reset seed entries bypass transformation.
    template<Nvic::IsrFunctionPointer Reset, typename Policy, typename TimeSource, typename... Ts>
    struct GetIsrPointersWithProfiling
      : Detail::CompileIsrPointerList<
          Nvic::InterruptOffsetTraits<void>::begin,
          brigand::list<Nvic::Isr<std::addressof(_LINKER_stack_end_), Nvic::Index<0>>,
                        Nvic::Isr<Reset, Nvic::Index<0>>>,
          typename TransformIsrList<
            Policy,
            TimeSource,
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::type> {
        // named constants: a short "due to requirement" line (the reports name the index)
        static constexpr bool uniqueIsrIndexes = Detail::UniqueIsrIndexes<
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value;
        static constexpr bool isrIndexesValid = ListRules::IsrIndexesValid<
          Nvic::InterruptOffsetTraits<void>,
          brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value;
        static_assert(
          uniqueIsrIndexes,
          "two peripherals in one Startup list claim the same interrupt vector: only the "
          "first would ever run (two DmaBase instances need an interruptInstance each)");
        static_assert(
          isrIndexesValid,
          "a peripheral installs an Isr on an index the chip's vector table does not have "
          "(outside InterruptOffsetTraits::begin..end, or a disabled index): it would be "
          "silently absent from the table");
    };

    template<Nvic::IsrFunctionPointer Reset, typename Policy, typename TimeSource, typename... Ts>
    using GetIsrPointersWithProfilingT =
      typename GetIsrPointersWithProfiling<Reset, Policy, TimeSource, Ts...>::type;

    namespace Detail {
        template<Nvic::IsrFunctionPointer Reset,
                 typename Policy,
                 typename TimeSource,
                 typename List>
        struct ProfiledIsrPointersOf;

        template<Nvic::IsrFunctionPointer Reset,
                 typename Policy,
                 typename TimeSource,
                 typename... Ts>
        struct ProfiledIsrPointersOf<Reset, Policy, TimeSource, brigand::list<Ts...>> {
            using type = GetIsrPointersWithProfilingT<Reset, Policy, TimeSource, Ts...>;
        };
    }   // namespace Detail

    namespace Detail {
        template<typename Primary, typename... Ts>
        struct BothCoresReports {
            static constexpr bool value
              = ((!IsSecondaryCore<Ts>::value
                  || ReportIndexes<
                    VectorOnBothCores,
                    Ts,
                    typename SharedVectorIndexes<
                      typename IsrsOf<Primary>::type,
                      typename IsrsOf<typename SecondaryPeripherals<Ts>::type>::type>::type>::value)
                 && ...);
        };
    }   // namespace Detail

    template<typename ClockSettings, typename... Peripherals>
    struct Startup {
        // The peripherals plus one generated dispatcher per vector their SubIsrs share
        // (SharedIsr.hpp); the list itself when none declares a SubIsr.
        using AllPeripherals = Detail::WithSharedDispatchers<Peripherals...>;

        // The list's shape (ListRules.hpp), before anything reads it. Each message names the entry.
        using NoDuplicates_ = ListRules::NoDuplicateEntry<brigand::list<Peripherals...>>;
        static constexpr bool noDuplicateEntry = NoDuplicates_::value;
        KVASIR_STATIC_ASSERT(
          noDuplicateEntry,
          (Detail::ListRuleText<Detail::DuplicateEntryRule,
                                typename NoDuplicates_::Duplicate>),
          "a peripheral is listed twice in one Startup list: its init steps would "
          "run twice");
        using AllPeripherals_
          = ListRules::AllArePeripherals<Detail::IsSecondaryCore, brigand::list<Peripherals...>>;
        static constexpr bool allArePeripherals = AllPeripherals_::value;
        KVASIR_STATIC_ASSERT(
          allArePeripherals,
          (Detail::ListRuleText<Detail::NotAPeripheralRule,
                                typename AllPeripherals_::NotAPeripheral>),
          "a type in the Startup list is not a peripheral (no init step, Isr, runtime hook, "
          "Provides/Claims or SecondaryCore): a driver's Config listed instead of the driver, "
          "or a typo'd alias - it would contribute nothing");
        static_assert(ListRules::AtMostOneSecondary<Detail::IsSecondaryCore,
                                                    brigand::list<Peripherals...>>::value,
                      "two SecondaryCores in one Startup list: the chip has one other core");
        using NoLaunchTimeout_ = ListRules::NoLaunchTimeoutIn<brigand::list<Peripherals...>>;
        static constexpr bool noLaunchTimeout = NoLaunchTimeout_::value;
        KVASIR_STATIC_ASSERT(
          noLaunchTimeout,
          (Detail::ListRuleText<Detail::LaunchTimeoutRule,
                                typename NoLaunchTimeout_::LaunchTimeout>),
          "a LaunchTimeout in the primary Startup list is ignored: it belongs in the "
          "SecondaryCore's list whose launch it bounds");
        using NoClockSettings_ = ListRules::NoClockSettingsIn<brigand::list<Peripherals...>>;
        static constexpr bool noClockSettings = NoClockSettings_::value;
        KVASIR_STATIC_ASSERT(
          noClockSettings,
          (Detail::ListRuleText<Detail::ClockSettingsRule,
                                typename NoClockSettings_::ClockSettings>),
          "a ClockSettings among the peripherals: its clock init never runs there "
          "(Startup's first argument is the one that does)");

        // Resources (Resources.hpp): what each peripheral provides and claims, checked over
        // this list and every SecondaryCore's list together. ClockSettings is in core 0's
        // list so it can provide the clock tree the drivers claim.
        using Resources = typename Detail::ResourceCheckOver<
          Detail::CoreLists<brigand::list<ClockSettings, Peripherals...>, Peripherals...>>::type;
        // The resource rules, reported per (peripheral, resource) - Diagnostics in
        // Resources.hpp; the plain asserts after it only fire if no report did.
        static_assert(Diagnostics::Diagnose<Resources>::value);
        static_assert(Resources::dependenciesListed,
                      "a peripheral needs another one listed (on core N, or in its own list) "
                      "that is not: ClockSync needs its Reference in core 0's list and its "
                      "Target in its own, LaunchTimeout its clock in core 0's");
        static_assert(
          ListRules::DependenciesPrecedeLaunch<Detail::IsSecondaryCore,
                                               Detail::SecondaryPeripherals,
                                               brigand::list<Peripherals...>>::value,
          "a SecondaryCore's peripheral needs a core 0 peripheral whose init has not run by "
          "the launch: list it before the SecondaryCore (ClockSync's Reference, "
          "LaunchTimeout's clock)");
        static constexpr bool noDoubleClaim_
          = Resources::noDoubleClaim;   // a short "due to requirement" line
        static_assert(noDoubleClaim_,
                      "a hardware resource is claimed by two peripherals (see the Report above)");
        static constexpr bool noDoubleProvide_ = Resources::noDoubleProvide;
        static_assert(noDoubleProvide_,
                      "a hardware resource is provided by two peripherals (see the Report above)");
        static constexpr bool claimsProvided_ = Resources::claimsProvided;
        static_assert(claimsProvided_,
                      "a peripheral claims a hardware resource nothing provides (see the Report "
                      "above)");
        static constexpr bool claimsLocal_ = Resources::claimsLocal;
        static_assert(claimsLocal_,
                      "a peripheral claims a hardware resource the other core's list provides "
                      "(see the Report above)");
        static constexpr bool coreAffinityHonoured_ = Resources::coreAffinityHonoured;
        static_assert(coreAffinityHonoured_,
                      "a peripheral that belongs to one core (its startupCore) is listed for "
                      "the other (see the Report above)");

        // The interrupt rules, reported per index (Detail::InterruptReports): a failure
        // names the index and the list in the instantiation trail. The plain asserts after
        // them are the same rules and only fire if a report did not.
        static_assert(Detail::InterruptReports<Startup,
                                               AllPeripherals>::value);
        static_assert(
          ListRules::EnabledLinesHandled<
            typename ListRules::EnabledInterruptsIn<AllPeripherals>::type,
            typename Detail::IsrsOf<AllPeripherals>::type>::value,
          "an init step enables an interrupt line for which nothing in this core's Startup "
          "list installs an Isr (see the Report above for the index)");

        static_assert(
          Detail::NoPeripheralOnBothCores<Peripherals...>::value,
          "a peripheral is listed for both cores: it belongs in exactly one Startup list");
        static_assert(Detail::noRuntimeInitAfterSecondaryCore<Peripherals...>(),
                      "nothing with a runtimeInit may follow a SecondaryCore: it would run "
                      "concurrently with the other core's main - list the SecondaryCore last");

        static_assert(Detail::BothCoresReports<AllPeripherals,
                                               Peripherals...>::value);
        static_assert(Detail::NoVectorOnBothCores<AllPeripherals,
                                                  Peripherals...>::value,
                      "an interrupt is installed in both cores' vector tables (see the Report "
                      "above for the index): list the peripheral on one core");

        // Every entry's ISR-context contract, the log backend's above all, against the enabled
        // interrupts of every core (ListRules::IsrContractHolds). Entries of a SecondaryCore's
        // list count, and so does the user backend when no list names it.
        using IsrCoreLists = Detail::CoreLists<brigand::list<Peripherals...>, Peripherals...>;
        using IsrContractEntries =
          typename Detail::IsrContractEntries<IsrCoreLists,
                                              ::uc_log::ComBackend<::uc_log::Tag::User>>::type;
        static_assert(Detail::NoContradictingIsrContract<IsrContractEntries>::value,
                      "an entry declares both isrPriorityLevels and singleIsrPriorityLevel");
        static_assert(Detail::IsrContractReports<IsrContractEntries,
                                                 IsrCoreLists>::value);
        static_assert(Detail::IsrContractsHold<IsrContractEntries,
                                               IsrCoreLists>::value,
                      "an entry's ISR contexts do not cover the enabled interrupts' priority "
                      "levels (see the Report above for the entry and the index): one ISR could "
                      "preempt another's log record. Serve the level, declare it silent, use "
                      "the masked-record mode, or, for an unknown level, set the priority with "
                      "a literal");

        // The reset entry (vector table word 1), named ResetISR in the image.
        [[KVASIR_RESETISR_ATTRIBUTES,
          gnu::noinline]] static void
        ResetISR() {
            Detail::nameResetEntry<&ResetISR>();
            Detail::StartupImplOf<ClockSettings, AllPeripherals>::type::ResetISR();
        }

        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<Detail::ExpandListT<BootIsrPointers<&ResetISR>::template type,
                                              AllPeripherals>> nvicIsrVectors{};

        // Every Extend the listed peripherals of both cores (a SecondaryCore's list included) added to Hook, in list
        // order, as a brigand::list: what a component that needs the list itself reads (Health::Supervisor).
        template<typename Hook>
        using ExtendsOn = typename Detail::FilterExtends<
          Hook,
          typename Detail::AllExtendsOfList<brigand::flatten<brigand::list<
            Peripherals...,
            typename Detail::SecondaryPeripherals<Peripherals>::type...>>>::type>::type;

        // Every function the listed peripherals added to Hook (their `Extends`, Hooks.hpp), in
        // list order. A hook nobody extends is an empty call.
        template<typename Hook,
                 typename... Args>
        [[gnu::always_inline]] static void run(Args const&... args) {
            Detail::runHookOf<Hook>(static_cast<brigand::list<Peripherals...>*>(nullptr), args...);
        }

        // The same calls as run<Hook>(), and when the loop is next needed: the earliest of what the functions
        // returned (Hooks.hpp Kvasir::Turn). For a loop that sleeps (Util/Executor.hpp).
        template<typename Hook,
                 typename... Args>
        [[gnu::always_inline]] static Kvasir::Turn runTurn(Args const&... args) {
            return Detail::runTurnOf<Hook>(static_cast<brigand::list<Peripherals...>*>(nullptr),
                                           args...);
        }

        // This core's list, for Executor::adopt and its boot report.
        using LocalPeripherals = brigand::list<Peripherals...>;
    };

    template<typename TimeSource>
    struct EnableTimeSourceHook {
        [[gnu::always_inline]] void operator()() const noexcept { TimeSource::enable(); }
    };

    // Primary template — only specialised for Startup<> below.
    template<typename BaseStartup,
             typename ProfilePolicy = ProfileNonePolicy,
             typename TimeSource    = DwtTimeSource>
    struct StartupWithProfiling;

    // Partial specialisation: unwraps Startup<ClockSettings, Peripherals...>
    // so callers only need to pass their existing Startup alias plus the two
    // profiling-specific arguments (policy and time source).
    template<typename ClockSettings,
             typename... Peripherals,
             typename ProfilePolicy,
             typename TimeSource>
    struct StartupWithProfiling<Startup<ClockSettings, Peripherals...>, ProfilePolicy, TimeSource> {
        // The reset entry (vector table word 1), named ResetISR in the image.
        [[KVASIR_RESETISR_ATTRIBUTES,
          gnu::noinline]] static void
        ResetISR() {
            Detail::nameResetEntry<&ResetISR>();
            Detail::StartupImplOf<ClockSettings,
                                  typename Startup<ClockSettings, Peripherals...>::AllPeripherals>::
              type::template ResetISR<EnableTimeSourceHook<TimeSource>>();
        }

        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<typename Detail::ProfiledIsrPointersOf<
            &ResetISR,
            ProfilePolicy,
            TimeSource,
            typename Startup<ClockSettings, Peripherals...>::AllPeripherals>::type>
            nvicIsrVectors{};

        // The full compiled ISR list (wrappers + plain Isr entries)
        using IsrList = typename Detail::ProfiledIsrPointersOf<
          &ResetISR,
          ProfilePolicy,
          TimeSource,
          typename Startup<ClockSettings, Peripherals...>::AllPeripherals>::type;

        // Only the IsrProfileWrapper entries
        using ProfiledWrapperList = FilterWrappersT<IsrList>;

        static constexpr std::size_t profiledCount = brigand::size<ProfiledWrapperList>::value;

        // Returns a fixed-size array of snapshots, one per profiled ISR.
        // Size is known at compile time — no heap allocation.
        static std::array<IsrProfileSnapshot,
                          profiledCount>
        getProfiles() noexcept {
            return getProfilesImpl(ProfiledWrapperList{});
        }

        static void printProfiles() {
            UC_LOG_T("{:#^32}", " ISR profiles "_sc);
            for([[maybe_unused]] auto const& p : getProfiles()) {
                UC_LOG_T("  isr[{:3}]  calls: {}", p.isrIndex, p.callCount);
                UC_LOG_T("    interval  min:{:>10}  avg:{:>10}  max:{:>10}  cyc",
                         p.minIntervalCycles,
                         p.avgIntervalCycles,
                         p.maxIntervalCycles);
                UC_LOG_T("    duration  min:{:>10}  avg:{:>10}  max:{:>10}  cyc",
                         p.minDurationCycles,
                         p.avgDurationCycles,
                         p.maxDurationCycles);
            }
        }

    private:
        template<typename... Wrappers>
        static std::array<IsrProfileSnapshot,
                          sizeof...(Wrappers)>
        getProfilesImpl(brigand::list<Wrappers...>) noexcept {
            return {IsrProfileStats<Wrappers::IType::value, TimeSource>::snapshot()...};
        }
    };

}}   // namespace Kvasir::Startup

#ifdef __arm__

namespace uc_log {
template<int Line,
         typename Filename,
         typename Expr>
inline void log_assert() {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_IMPL(uc_log::LogLevel::crit,
                Line,
                std::string_view{Filename{}()},
                sc::escape(
                  sc::create([]() { return std::string_view{Expr{}()}; }),
                  [](auto c) { return c == '{' || c == '}'; },
                  [](auto c) { return c; }));
}

    // llvm-libc's and libc++'s patched headers already declare log_assert (-Wredundant-decls);
    // without any prototype clang's -Wmissing-prototypes objects instead
    #if !defined(LIBC_NAMESPACE) && !defined(_LIBCPP_VERSION)
void log_assert(int         line,
                char const* filename,
                char const* expr);
    #endif

void log_assert([[maybe_unused]] int         line,
                [[maybe_unused]] char const* filename,
                [[maybe_unused]] char const* expr) {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("libc/libc++ assert({}) {}:{}",
             std::string_view{expr},
             std::string_view{filename},
             line);
}

}   // namespace uc_log

namespace Kvasir::Panic {
// The one definition (this header is the TU with main's): noinline so the return address is the
// caller's, the handler looked up in dispatch<>'s instantiation, after the application's.
[[noreturn,
  gnu::noinline,
  gnu::used]] void
raise(Cause cause) {
    Detail::dispatch(Info{cause, reinterpret_cast<std::uint32_t>(__builtin_return_address(0))});
}

// A library function's panic on its caller's behalf: the site the caller passes (Panic.hpp).
[[noreturn,
  gnu::noinline,
  gnu::used]] void
raiseAt(Cause         cause,
        std::uint32_t pc,
        std::uint32_t detail) {
    Detail::dispatch(Info{cause, pc, detail});
}
}   // namespace Kvasir::Panic

    // newlib's assert() pulls stdio/_write/_sbrk and libstdc++'s __throw_* pull abort -> malloc;
    // strong definitions here keep those archive members out. gnu::noreturn, not [[noreturn]]:
    // the standard attribute must be on newlib's first declaration.
    #if defined(__NEWLIB__)
extern "C" {
[[gnu::noreturn,
  gnu::used]] void
__assert_func(char const* file,
              int         line,
              char const* /*func*/,
              char const* expr) {
    uc_log::log_assert(line, file, expr);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::assertion,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
}

[[gnu::noreturn,
  gnu::used]] void
abort() {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("abort() called (libstdc++ __throw_* or libc)");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
}

void _exit(int);   // newlib declares it in <unistd.h>, which is not included here

[[gnu::noreturn,
  gnu::used]] void
_exit(int) {
    abort();
}
}
    #endif
    #if defined(__NEWLIB__)
// gcc has no [[clang::no_destroy]]; accept and ignore the atexit() registration of static
// destructors - firmware never exits. newlib's own atexit() would also link register_fini ->
// __libc_fini_array -> _fini, which lives in crti.o and is not linked (clang with newlib).
extern "C" {
[[gnu::used]] int atexit(void (*)()) { return 0; }
}
    #elif defined(LIBC_NAMESPACE)
// Whatever the compiler (gcc with llvm-libc too: it used to take the newlib branch above and link without abort/exit).
// third-party static destructors; noexcept as llvm-libc declares it
extern "C" {
[[gnu::used]] int atexit(void (*)()) noexcept { return 0; }

// llvm-libc's baremetal build has no abort(). The same end as newlib's.
[[gnu::noreturn,
  gnu::used]] void
abort() noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("abort() called");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
}

// Nor exit() / _Exit().
// A firmware has nowhere to return to: the same end as abort(), with the status in the line.
[[gnu::noreturn,
  gnu::used]] void
exit(int status) noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("exit({}) called", status);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)),
                           static_cast<std::uint32_t>(status));
}

[[gnu::noreturn,
  gnu::used]] void
_Exit(int status) noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("_Exit({}) called", status);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)),
                           static_cast<std::uint32_t>(status));
}
}
    #endif
    #if defined(KVASIR_HEAP) && defined(__NEWLIB__)
// The full newlib (gcc's speed variants) links its stdio as soon as malloc is used: malloc needs the reent structure,
// which there holds the three standard streams. Nothing opens or prints through them, but their read/write/seek/close
// need a system call each, and libnosys' stand-ins are linker warnings ("_write is not implemented and will always
// fail"). These fail the same way, silently.
extern "C" {
int _close(int);
int _lseek(int,
           int,
           int);
int _read(int,
          char*,
          int);
int _write(int,
           char const*,
           int);

[[gnu::used]] int _close(int) { return -1; }

[[gnu::used]] int _lseek(int,
                         int,
                         int) {
    return -1;
}

[[gnu::used]] int _read(int,
                        char*,
                        int) {
    return -1;
}

[[gnu::used]] int _write(int,
                         char const*,
                         int) {
    return -1;
}
}

// A gcc build with HEAP_SIZE: newlib's malloc takes its memory through _sbrk - out of the linker's .heap, never past
// its end (without this the link fails on "undefined reference to `_sbrk'").
extern "C" {
void* _sbrk(std::ptrdiff_t increment);

[[gnu::used]] void* _sbrk(std::ptrdiff_t increment) {
    static std::uintptr_t brk{};
    auto const            start = reinterpret_cast<std::uintptr_t>(&_LINKER_heap_start_);
    auto const            end   = reinterpret_cast<std::uintptr_t>(&_LINKER_heap_end_);
    if(brk == 0) { brk = start; }
    auto const next
      = brk
      + static_cast<std::uintptr_t>(increment);   // modulo 2^32: a negative increment gives back
    if(next < start || next > end) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(std::uintptr_t{0} - 1U);   // (void*)-1, newlib's "no memory"
    }
    auto const previous = brk;
    brk                 = next;
    return reinterpret_cast<void*>(previous);
}
}
    #endif
    #if defined(KVASIR_HEAP) && defined(LIBC_NAMESPACE)
// llvm-libc's heap aligns every block to max(4, alignof(max_align_t)) (Block::MIN_ALIGN), and
// operator new(size_t) promises __STDCPP_DEFAULT_NEW_ALIGNMENT__. A max_align_t weaker than that
// (lib/libc/include/stddef.h once had `typedef int`) hands 8-aligned types 4-aligned
// memory: a sanitizer type_mismatch at every such new.
static_assert(alignof(std::max_align_t) >= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
              "max_align_t is weaker than operator new's default alignment: the heap would return "
              "misaligned blocks (check the libc's stddef.h)");
// llvm-libc baremetal OSUtil hooks: heap corruption is reported via stderr and exit
extern "C" {
struct __llvm_libc_stdio_cookie {
    char unused;
};

extern __llvm_libc_stdio_cookie        __llvm_libc_stdin_cookie;
extern __llvm_libc_stdio_cookie        __llvm_libc_stdout_cookie;
extern __llvm_libc_stdio_cookie        __llvm_libc_stderr_cookie;
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie{};
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie{};
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie{};

long                   __llvm_libc_stdio_read(void*,
                                              char*,
                                              std::size_t);
long                   __llvm_libc_stdio_write(void*,
                                               char const*,
                                               std::size_t);
[[gnu::noreturn]] void __llvm_libc_exit(int);

[[gnu::used]] long __llvm_libc_stdio_read(void*,
                                          char*,
                                          std::size_t) {
    return 0;
}

[[gnu::used]] long __llvm_libc_stdio_write(void*,
                                           char const* buf,
                                           std::size_t size) {
    UC_LOG_SCOPE_MODULE("libc");
    UC_LOG_C("libc: {}", std::string_view{buf, size});
    return static_cast<long>(size);
}

[[gnu::noreturn,
  gnu::used]] void
__llvm_libc_exit(int status) {
    UC_LOG_SCOPE_MODULE("libc");
    UC_LOG_C("libc exit({})", status);
    Kvasir::Panic::raise(Kvasir::Panic::Cause::abort);
}
}
    #endif
    #if defined(__GLIBCXX__)
// libstdc++'s _GLIBCXX_ASSERTIONS reporter (the sanitize variant turns those on)
namespace std {
[[gnu::noreturn,
  gnu::used]] void
__glibcxx_assert_fail(char const* file,
                      int         line,
                      char const* /*func*/,
                      char const* cond) noexcept {
    uc_log::log_assert(line, file, cond);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::assertion,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
}
}   // namespace std
    #endif

    #if defined(__clang__) && defined(KVASIR_COMPILER_RT_LIBGCC) && defined(LIBC_NAMESPACE)
// clang lowers struct copies and memset to the AEABI helpers, which neither libgcc nor llvm-libc
// provides. Argument order matters: __aeabi_memset takes (dest, n, value), the reverse of memset.
extern "C" {
[[gnu::used]] inline void __aeabi_memcpy(void*       d,
                                         void const* s,
                                         std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy4(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy8(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove4(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove8(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memset(void*       d,
                                         std::size_t n,
                                         int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset4(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset8(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memclr(void*       d,
                                         std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr4(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr8(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}
}
    #endif

extern "C" {
// EHABI personality routines. Nothing here unwinds, but the sanitizers make clang emit .ARM.exidx
// entries whose personality reference would pull libgcc's whole unwinder out of the archive;
// defining the routines keeps it out.
[[gnu::used]] inline void __aeabi_unwind_cpp_pr0() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr1() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr2() {}

[[noreturn,
  gnu::used]] inline void
__stack_chk_fail() {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("stack smashed: a -fstack-protector canary was overwritten");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::stackSmash,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
}
}

[[noreturn]] inline void Kvasir::Nvic::DefaultIsrs::onIsr() {
    std::uint32_t ipsr{};
    asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    auto const irq = static_cast<std::int32_t>(ipsr) - 16;
    UC_LOG_C("unhandled interrupt fired, IRQ={}", irq);
    // no program site caused it (pc 0): the record names the interrupt instead - the exception
    // number in detail, IRQ = detail - 16, a negative IRQ a system exception (IPSR, Armv6-M ARM
    // DDI0419E B1.4.2 / Armv8-M ARM DDI0553B.y D1.2.137)
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::unhandledInterrupt, 0, ipsr);
}

#endif

namespace Kvasir::Startup {
// The one line of a firmware that names its Startup type, after the type:
//     template struct Kvasir::Startup::Start<Startup>;
// The explicit instantiation instantiates `vectors`, which odr-uses the vector table (a
// static member of a class template exists only once something uses it) and through it
// the reset entry. Forgetting it leaves the image without a vector table, which the link
// refuses (common.ld asserts that ResetISR is defined).
template<typename S>
struct Start {
#ifdef __arm__
    static constexpr auto const* vectors = std::addressof(S::nvicIsrVectors);
#endif
};
}   // namespace Kvasir::Startup

extern "C" {
[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int);

[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int) {
#ifdef __arm__
    UC_LOG_C("integer division by zero");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::divideByZero,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
#else
    assert(false);
#endif
}

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long);

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long) {
#ifdef __arm__
    UC_LOG_C("64-bit integer division by zero");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::divideByZero,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
#else
    assert(false);
#endif
}
}

namespace std {
//void terminate() noexcept { assert(false); }
}   // namespace std

#if !defined(KVASIR_HEAP)
void operator delete(void*) noexcept {}

void operator delete(void*,
                     std::size_t) noexcept {}

void operator delete[](void*) noexcept {}

void operator delete[](void*,
                       std::size_t) noexcept {}

    // keep libc++'s new.cpp (and its malloc) out of the link; reaching one is a bug
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wmissing-noreturn"

void* operator new(std::size_t) {
    #ifdef __arm__
    UC_LOG_C("operator new without a heap (HEAP_SIZE)");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::allocation,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
    #else
    assert(false);
    __builtin_trap();
    #endif
}

void* operator new[](std::size_t) {
    #ifdef __arm__
    UC_LOG_C("operator new[] without a heap (HEAP_SIZE)");
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::allocation,
                           reinterpret_cast<std::uint32_t>(__builtin_return_address(0)));
    #else
    assert(false);
    __builtin_trap();
    #endif
}

    #pragma GCC diagnostic pop
#endif

#if defined(KVASIR_HEAP) && defined(__NEWLIB__)
// A gcc build with a heap. libstdc++'s own operator new throws std::bad_alloc when malloc returns null, and that one
// throw links the unwinder (__exidx_start / __exidx_end, which no script here defines), the verbose terminate handler
// and newlib's stdio behind it ("_write is not implemented and will always fail", ...: gcc's speed
// variants). These replace it: straight to newlib's malloc, and an exhausted heap is the allocation
// panic, as an allocation without a heap is above.
namespace Kvasir::Startup::Detail {
[[gnu::always_inline]] inline void* allocateOrPanic(void* block,
                                                    void* caller) {
    if(block == nullptr) {
        UC_LOG_C("operator new: the heap (HEAP_SIZE) is exhausted");
        Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::allocation,
                               reinterpret_cast<std::uint32_t>(caller));
    }
    return block;
}
}   // namespace Kvasir::Startup::Detail

void* operator new(std::size_t size) {
    return Kvasir::Startup::Detail::allocateOrPanic(std::malloc(size == 0 ? 1 : size),
                                                    __builtin_return_address(0));
}

void* operator new[](std::size_t size) {
    return Kvasir::Startup::Detail::allocateOrPanic(std::malloc(size == 0 ? 1 : size),
                                                    __builtin_return_address(0));
}

void operator delete(void* block) noexcept { std::free(block); }

void operator delete(void* block,
                     std::size_t) noexcept {
    std::free(block);
}

void operator delete[](void* block) noexcept { std::free(block); }

void operator delete[](void* block,
                       std::size_t) noexcept {
    std::free(block);
}

// new (std::nothrow): libstdc++'s wraps the throwing one in a try block (the personality routine, the unwinder)
void* operator new(std::size_t size,
                   std::nothrow_t const&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size,
                     std::nothrow_t const&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* block,
                     std::nothrow_t const&) noexcept {
    std::free(block);
}

void operator delete[](void* block,
                       std::nothrow_t const&) noexcept {
    std::free(block);
}
#endif

#if defined(__NEWLIB__) && defined(__GLIBCXX__) && defined(__arm__)
// libstdc++'s error paths (a container asked for too much, vector::at out of range, bad_alloc) end in its
// std::__throw_* helpers, all fifteen in one object of the library (functexcept.o). In the full library - gcc's speed
// variants - they really throw: the unwinder, the type-info tables, the verbose terminate handler with its demangler
// and newlib's stdio come with that one object, and the image does not link (__exidx_start; "_write is not implemented
// and will always fail"). Defined here, the linker never takes that object: each helper
// logs what would have been thrown and raises the abort panic at the caller. Under their mangled names, as the
// __sync_* shims are, so no declaration here has to match the library's. All fifteen, or the linker takes the object
// for the missing one and reports the others as defined twice (the list is the same in libstdc++ 16.2's full and
// nano libraries, v6-M and v8-M).
namespace Kvasir::Startup::Detail {
[[gnu::noreturn,
  gnu::always_inline]] inline void
libstdcxxThrow(std::string_view exception,
               char const*      what,
               void*            caller) {
    UC_LOG_SCOPE_MODULE("assert");
    UC_LOG_C("libstdc++ would throw std::{}: {}",
             exception,
             std::string_view{what == nullptr ? "" : what});
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::abort, reinterpret_cast<std::uint32_t>(caller));
}

    #define KVASIR_LIBSTDCXX_THROW(function, mangled, exception)             \
        [[gnu::noreturn, gnu::used]] void function() __asm__(mangled);       \
        [[gnu::noreturn, gnu::used]] void function() {                       \
            libstdcxxThrow(exception, nullptr, __builtin_return_address(0)); \
        }
    #define KVASIR_LIBSTDCXX_THROW_WHAT(function, mangled, exception)                  \
        [[gnu::noreturn, gnu::used]] void function(char const* what) __asm__(mangled); \
        [[gnu::noreturn, gnu::used]] void function(char const* what) {                 \
            libstdcxxThrow(exception, what, __builtin_return_address(0));              \
        }
KVASIR_LIBSTDCXX_THROW(throwBadCast,
                       "_ZSt16__throw_bad_castv",
                       "bad_cast")
KVASIR_LIBSTDCXX_THROW(throwBadAlloc,
                       "_ZSt17__throw_bad_allocv",
                       "bad_alloc")
KVASIR_LIBSTDCXX_THROW(throwBadTypeid,
                       "_ZSt18__throw_bad_typeidv",
                       "bad_typeid")
KVASIR_LIBSTDCXX_THROW(throwBadException,
                       "_ZSt21__throw_bad_exceptionv",
                       "bad_exception")
KVASIR_LIBSTDCXX_THROW(throwBadArrayNewLength,
                       "_ZSt28__throw_bad_array_new_lengthv",
                       "bad_array_new_length")
KVASIR_LIBSTDCXX_THROW_WHAT(throwLogicError,
                            "_ZSt19__throw_logic_errorPKc",
                            "logic_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwRangeError,
                            "_ZSt19__throw_range_errorPKc",
                            "range_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwDomainError,
                            "_ZSt20__throw_domain_errorPKc",
                            "domain_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwLengthError,
                            "_ZSt20__throw_length_errorPKc",
                            "length_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwOutOfRange,
                            "_ZSt20__throw_out_of_rangePKc",
                            "out_of_range")
KVASIR_LIBSTDCXX_THROW_WHAT(throwRuntimeError,
                            "_ZSt21__throw_runtime_errorPKc",
                            "runtime_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwOverflowError,
                            "_ZSt22__throw_overflow_errorPKc",
                            "overflow_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwUnderflowError,
                            "_ZSt23__throw_underflow_errorPKc",
                            "underflow_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwInvalidArgument,
                            "_ZSt24__throw_invalid_argumentPKc",
                            "invalid_argument")
    #undef KVASIR_LIBSTDCXX_THROW
    #undef KVASIR_LIBSTDCXX_THROW_WHAT

// the one with a printf format: the format itself is logged, its arguments are not expanded
[[gnu::noreturn,
  gnu::used]] void
throwOutOfRangeFmt(char const* format,
                   ...) __asm__("_ZSt24__throw_out_of_range_fmtPKcz");

[[gnu::noreturn,
  gnu::used]] void
throwOutOfRangeFmt(char const* format,
                   ...) {
    libstdcxxThrow("out_of_range", format, __builtin_return_address(0));
}
}   // namespace Kvasir::Startup::Detail
#endif

#include "kvasir/StartUp/SecondaryCore.hpp"
