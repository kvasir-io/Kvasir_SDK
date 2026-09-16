#pragma once

#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Utility.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/IsrProfiler.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/Util/attributes.hpp"
#include "kvasir/Util/ubsan.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string_view>

// declaring and calling main is ill-formed in ISO C++ but intended here; gcc diagnoses it under -Wpedantic
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern "C" {
[[KVASIR_RESETISR_ATTRIBUTES]] extern void ResetISR();
extern int                                 main();
}
#pragma GCC diagnostic pop

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
        static_assert(
          Detail::UniqueIsrIndexes<
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value,
          "two peripherals in one Startup list claim the same interrupt vector: only the "
          "first would ever run (two DmaBase instances need an interruptInstance each)");
        static_assert(
          ListRules::IsrIndexesValid<
            Nvic::InterruptOffsetTraits<void>,
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value,
          "a peripheral installs an Isr on an index the chip's vector table does not have "
          "(outside InterruptOffsetTraits::begin..end, or a disabled index): it would be "
          "silently absent from the table");
    };

    template<Nvic::IsrFunctionPointer StackEnd, Nvic::IsrFunctionPointer Reset, typename... Ts>
    using GetIsrPointersForT = typename GetIsrPointersFor<StackEnd, Reset, Ts...>::type;

    template<typename... Ts>
    struct GetIsrPointers
      : GetIsrPointersFor<std::addressof(_LINKER_stack_end_), ResetISR, Ts...> {};

    template<typename... Ts>
    using GetIsrPointersT = typename GetIsrPointers<Ts...>::type;

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

        // The interrupt rules' reports (Diagnostics::Report, Resources.hpp): each names the
        // index in its instantiation. `Where` is the list's marker type, Startup or a
        // SecondaryCore, so the trail also says which core.
        struct VectorTwice {
            static constexpr std::string_view message
              = "two peripherals in one Startup list claim the same interrupt vector: only "
                "the first would ever run (two DmaBase instances need an interruptInstance "
                "each)";
        };

        struct VectorOnBothCores {
            static constexpr std::string_view message
              = "an interrupt is installed in both cores' vector tables: the ISR would run "
                "on both cores at once (only the SIO, IO_BANK0 and core-exception vectors "
                "are per core): list the peripheral on one core";
        };

        struct VectorOutOfTable {
            static constexpr std::string_view message
              = "a peripheral installs an Isr on an index the chip's vector table does not "
                "have (outside InterruptOffsetTraits::begin..end, or a disabled index): it "
                "would be silently absent from the table";
        };

        struct EnabledUnhandled {
            static constexpr std::string_view message
              = "an init step enables an interrupt line for which nothing in this core's "
                "list installs an Isr: the line would fire into the unhandled-interrupt "
                "handler at its first event (a driver whose Isr alias was dropped, or an "
                "interruptEnable copied from another driver)";
        };

        // An entry's ISR-context contract (ListRules.hpp: IsrContractHolds) against the enabled
        // interrupts, reported per (entry, index).
        struct IsrLevelUnserved {
            static constexpr std::string_view message
              = "an interrupt is enabled at a priority level this entry has no ISR context "
                "for: an ISR at that level could preempt another ISR's log record on a ring "
                "it shares. Give the level its own context (uc_log: add it to the "
                "IsrPolicy::PerLevel Levels<...>, preferred), declare it silent if no ISR at "
                "that level ever logs (SilentLevels<...>), or use the entry's masked-record "
                "mode (IsrPolicy::MaskedRecord)";
        };

        struct IsrLevelsDiffer {
            static constexpr std::string_view message
              = "enabled interrupts that may log use more than one priority level, and this "
                "entry keeps one ISR context for all of them: an ISR at the higher level "
                "preempts one at the lower mid-record. Configure the entry per level (uc_log: "
                "IsrPolicy::PerLevel, preferred), declare the levels whose ISRs never log as "
                "silent (SilentLevels<...>, SysTick's level 0 typically), or use its "
                "masked-record mode (IsrPolicy::MaskedRecord)";
        };

        struct IsrLevelUnknown {
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

        struct NoOpStartupHook {
            [[gnu::always_inline]] void operator()() const noexcept {}
        };

        // Shared ResetISR body. Hook is called immediately after FirstInitStep,
        // before any ISR fires — used by StartupWithProfiling to enable the
        // DWT cycle counter; NoOpStartupHook for plain Startup.
        template<typename ClockSettings, typename... Peripherals>
        struct StartupImpl {
            template<typename Hook = NoOpStartupHook>
            [[noreturn,
              gnu::always_inline]] static void
            ResetISR() {
                FirstInitStep<Kvasir::Tag::User>{}();
                Hook{}();

                Kvasir::Register::apply(GetEarlyInitT<Peripherals...>{});

                ClockSettings::coreClockInit();

                initMemory();

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
    // Stack-end and ResetISR seed entries bypass transformation.
    template<typename Policy, typename TimeSource, typename... Ts>
    struct GetIsrPointersWithProfiling
      : Detail::CompileIsrPointerList<
          Nvic::InterruptOffsetTraits<void>::begin,
          brigand::list<Nvic::Isr<std::addressof(_LINKER_stack_end_), Nvic::Index<0>>,
                        Nvic::Isr<ResetISR, Nvic::Index<0>>>,
          typename TransformIsrList<
            Policy,
            TimeSource,
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::type> {
        static_assert(
          Detail::UniqueIsrIndexes<
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value,
          "two peripherals in one Startup list claim the same interrupt vector: only the "
          "first would ever run (two DmaBase instances need an interruptInstance each)");
        static_assert(
          ListRules::IsrIndexesValid<
            Nvic::InterruptOffsetTraits<void>,
            brigand::flatten<brigand::list<typename Detail::ExtractIsr<Ts>::type...>>>::value,
          "a peripheral installs an Isr on an index the chip's vector table does not have "
          "(outside InterruptOffsetTraits::begin..end, or a disabled index): it would be "
          "silently absent from the table");
    };

    template<typename Policy, typename TimeSource, typename... Ts>
    using GetIsrPointersWithProfilingT =
      typename GetIsrPointersWithProfiling<Policy, TimeSource, Ts...>::type;

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
        // The list's shape (ListRules.hpp), before anything reads it.
        static_assert(ListRules::NoDuplicateEntry<brigand::list<Peripherals...>>::value,
                      "a peripheral is listed twice in one Startup list: its init steps would "
                      "run twice");
        static_assert(
          ListRules::AllArePeripherals<Detail::IsSecondaryCore,
                                       brigand::list<Peripherals...>>::value,
          "a type in the Startup list is not a peripheral (no init step, Isr, runtime hook, "
          "Provides/Claims or SecondaryCore): a driver's Config listed instead of the driver, "
          "or a typo'd alias - it would contribute nothing");
        static_assert(ListRules::AtMostOneSecondary<Detail::IsSecondaryCore,
                                                    brigand::list<Peripherals...>>::value,
                      "two SecondaryCores in one Startup list: the chip has one other core");
        static_assert(ListRules::NoLaunchTimeoutIn<brigand::list<Peripherals...>>::value,
                      "a LaunchTimeout in the primary Startup list is ignored: it belongs in the "
                      "SecondaryCore's list whose launch it bounds");
        static_assert(ListRules::NoClockSettingsIn<brigand::list<Peripherals...>>::value,
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
        static_assert(Resources::noDoubleClaim,
                      "a hardware resource is claimed by two peripherals (see the Report above)");
        static_assert(Resources::noDoubleProvide,
                      "a hardware resource is provided by two peripherals (see the Report above)");
        static_assert(Resources::claimsProvided,
                      "a peripheral claims a hardware resource nothing provides (see the Report "
                      "above)");
        static_assert(Resources::claimsLocal,
                      "a peripheral claims a hardware resource the other core's list provides "
                      "(see the Report above)");
        static_assert(Resources::coreAffinityHonoured,
                      "a peripheral that belongs to one core (its startupCore) is listed for "
                      "the other (see the Report above)");

        // The interrupt rules, reported per index (Detail::InterruptReports): a failure
        // names the index and the list in the instantiation trail. The plain asserts after
        // them are the same rules and only fire if a report did not.
        static_assert(Detail::InterruptReports<Startup,
                                               brigand::list<Peripherals...>>::value);
        static_assert(
          ListRules::EnabledLinesHandled<
            typename ListRules::EnabledInterruptsIn<brigand::list<Peripherals...>>::type,
            typename Detail::IsrsOf<brigand::list<Peripherals...>>::type>::value,
          "an init step enables an interrupt line for which nothing in this core's Startup "
          "list installs an Isr (see the Report above for the index)");

        static_assert(
          Detail::NoPeripheralOnBothCores<Peripherals...>::value,
          "a peripheral is listed for both cores: it belongs in exactly one Startup list");
        static_assert(Detail::noRuntimeInitAfterSecondaryCore<Peripherals...>(),
                      "nothing with a runtimeInit may follow a SecondaryCore: it would run "
                      "concurrently with the other core's main - list the SecondaryCore last");

        static_assert(Detail::BothCoresReports<brigand::list<Peripherals...>,
                                               Peripherals...>::value);
        static_assert(Detail::NoVectorOnBothCores<brigand::list<Peripherals...>,
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

        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<Kvasir::Startup::GetIsrPointersT<Peripherals...>> nvicIsrVectors{};

        [[noreturn,
          gnu::always_inline]] static void
        ResetISR() {
            Detail::StartupImpl<ClockSettings, Peripherals...>::ResetISR();
        }
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
        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Kvasir::Startup::
          NvicVectorTable<GetIsrPointersWithProfilingT<ProfilePolicy, TimeSource, Peripherals...>>
            nvicIsrVectors{};

        [[noreturn,
          gnu::always_inline]] static void
        ResetISR() {
            Detail::StartupImpl<ClockSettings, Peripherals...>::template ResetISR<
              EnableTimeSourceHook<TimeSource>>();
        }

        // The full compiled ISR list (wrappers + plain Isr entries)
        using IsrList = GetIsrPointersWithProfilingT<ProfilePolicy, TimeSource, Peripherals...>;

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
    UC_LOG_C("libc/libc++ assert({}) {}:{}",
             std::string_view{expr},
             std::string_view{filename},
             line);
}

}   // namespace uc_log

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
    while(true) { asm volatile("bkpt 5" : : :); }
}

[[gnu::noreturn,
  gnu::used]] void
abort() {
    UC_LOG_C("abort() called (libstdc++ __throw_* or libc)");
    while(true) { asm volatile("bkpt 5" : : :); }
}

void _exit(int);   // newlib declares it in <unistd.h>, which is not included here

[[gnu::noreturn,
  gnu::used]] void
_exit(int) {
    abort();
}
}
    #endif
    #if !defined(__clang__)
// gcc has no [[clang::no_destroy]]; accept and ignore the atexit() registration of static
// destructors - firmware never exits
extern "C" {
[[gnu::used]] int atexit(void (*)()) { return 0; }
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
    while(true) { asm volatile("bkpt 5" : : :); }
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

[[gnu::used]] inline constexpr std::uint32_t __stack_chk_guard{0xdeadc0de};

[[noreturn,
  gnu::used]] inline void
__stack_chk_fail() {
    assert(false);
}
}

[[noreturn]] inline void Kvasir::Nvic::DefaultIsrs::onIsr() {
    UC_LOG_C("unhandled interrupt fired, IRQ={}", []() {
        std::uint32_t ipsr_val{};
        asm volatile("mrs %0, ipsr" : "=r"(ipsr_val));
        return static_cast<std::int32_t>(ipsr_val) - 16;
    }());
    while(true) { asm volatile("bkpt 7" : : :); }
}

    #define KVASIR_START(Startup)                        \
        [[KVASIR_RESETISR_ATTRIBUTES]] void ResetISR() { \
            (void)Startup::nvicIsrVectors.data[1];       \
            Startup::ResetISR();                         \
        }
#else
    #define KVASIR_START(Startup)   // TODO
#endif

extern "C" {
[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int);

[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int) {
    assert(false);
}

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long);

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long) {
    assert(false);
}
}

namespace std {
//void terminate() noexcept { assert(false); }
}   // namespace std

void operator delete(void*) noexcept {}

void operator delete(void*,
                     std::size_t) noexcept {}

#include "kvasir/StartUp/SecondaryCore.hpp"
