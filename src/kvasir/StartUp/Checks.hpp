#pragma once

// The compile-time checks of a Startup list and of a SecondaryCore's list, each rule once.
//
// A rule is a text (`message`, plus a `kind` when its number is an interrupt or a core) and an
// offender list: brigand::list<Diagnostics::Pair<Who, What>...>. Each offender is one
// Diagnostics::Report, whose static_assert names it; a rule holds when its list is empty, so
// there is no second, boolean form that could disagree, and no backstop assert. The rules'
// views that the host tests and other headers read (ResourceCheck::noDoubleClaim,
// ListRules::NoDuplicateEntry, ListRules::IsrContractHolds, ...) are the same lists.
//
//   CheckList<Ctx, Role, Core, Where, Entries, WithDispatchers>   one core's list
//     hook shapes, a type twice, not a peripheral, (primary) two SecondaryCores, a LaunchTimeout
//     outside a SecondaryCore, the user log backend without a boot report, (secondary) a SecondaryCore inside one, a ClockSettings among the
//     peripherals, a BootGuard after the boot report; the vector rules (twice, outside the table,
//     enabled without an Isr) over the list with SharedIsr's dispatchers. Shape errors read
//     `<message>: core N - <Type>`. Entries: the list's entries, its groups replaced by their members.
//   ImageChecks<Ctx, Where, ClockSettings, Entries, WithDispatchers>   the whole image, from
//     the primary list: the resource rules (Resources.hpp), dependencies before the launch, a
//     peripheral on both cores, a runtimeInit after the SecondaryCore, a vector on both cores,
//     the ISR-context contracts, the image's CRC engine set in no list while it claims hardware.
//
// Host-compilable: everything chip-specific comes in through the Context (the vector table's
// traits, what a SecondaryCore is, the log backend whose contract counts unlisted).

#include "kvasir/Mpl/List.hpp"
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/Util/Diagnostic.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace Kvasir { namespace Startup { namespace Detail {
    // ---- an entry's ISRs ------------------------------------------------------------------
    // `using Isr = Nvic::Isr<&handler, Index>;` (one ISR, or a list of them) is the one spelling.
    template<typename T>
    struct ExtractIsr : brigand::list<> {};

    template<typename T>
        requires requires { typename T::Isr; }
    struct ExtractIsr<T> : T::Isr {};

    // A list read through Entries<...> (Entry.hpp): its groups are already their members.
    template<typename List>
    struct IsrsOf;

    template<typename... Ts>
    struct IsrsOf<brigand::list<Ts...>> {
        using type = brigand::flatten<brigand::list<typename ExtractIsr<Ts>::type...>>;
    };

    // A vector installed in both cores' tables. Most interrupt lines are chip-wide and
    // reach both NVICs: enable one on both cores and the ISR runs on both cores at
    // once, each clearing the flag the other was about to look at. The lines that are
    // legitimately per core (SIO FIFO and doorbells, IO_BANK0, the core exceptions) are
    // the chip's `InterruptOffsetTraits::perCore`; a chip that lists none has none.
    template<typename Traits>
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
    constexpr bool vectorAlsoIn(brigand::list<Is...>) {
        return (false || ... || std::is_same_v<typename I::IType, typename Is::IType>);
    }

    // The vectors a list installs: its entries' ISRs and their sub-interrupts (kvasir/StartUp/SharedIsr.hpp: Startup
    // generates one dispatcher per vector from them, each Sub names its vector as IType). Core 0's list is passed with
    // its generated dispatchers already; a SecondaryCore's entries come as written, so its sub-interrupts count here
    // (an entry on a chip-wide vector in both lists is the same mistake whether it is an Isr or a SubIsrs).
    template<typename T>
    struct SubIsrVectorsOf {
        using type = brigand::list<>;
    };

    template<typename T>
        requires requires { typename T::SubIsrs; }
    struct SubIsrVectorsOf<T> {
        using type = typename T::SubIsrs;
    };

    template<typename List>
    struct VectorsOf;

    template<typename... Ts>
    struct VectorsOf<brigand::list<Ts...>> {
        using type = brigand::flatten<brigand::list<typename IsrsOf<brigand::list<Ts...>>::type,
                                                    typename SubIsrVectorsOf<Ts>::type...>>;
    };

    // The indexes core 0 and a SecondaryCore both install, chip-wide ones only.
    template<typename Traits, typename Primary, typename Secondary>
    struct SharedVectorIndexes;

    template<typename Traits, typename... As, typename Secondary>
    struct SharedVectorIndexes<Traits, brigand::list<As...>, Secondary> {
        using type = brigand::flatten<brigand::list<ListRules::Detail::IndexIf<
          (!isPerCoreVector<Traits>(As::IType::value) && vectorAlsoIn<As>(Secondary{})),
          As::IType::value>...>>;
    };

    // The per-core peripheral lists the image rules run over, core 0's first and then one per
    // SecondaryCore, in list order. The list index is the core number, which is what the
    // affinity rule (startupCore) compares against - so entries that are not a SecondaryCore
    // contribute no list at all, rather than an empty one that would push core 1's list to
    // some other index.
    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename Out,
             typename... Ts>
    struct CoreListsImpl;

    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename... Os>
    struct CoreListsImpl<IsSecondaryCore, SecondaryPeripherals, brigand::list<Os...>> {
        using type = brigand::list<Os...>;
    };

    template<template<typename> class IsSecondaryCore,
             template<typename> class SecondaryPeripherals,
             typename... Os,
             typename T,
             typename... Ts>
    struct CoreListsImpl<IsSecondaryCore, SecondaryPeripherals, brigand::list<Os...>, T, Ts...>
      : CoreListsImpl<
          IsSecondaryCore,
          SecondaryPeripherals,
          std::conditional_t<IsSecondaryCore<T>::value,
                             brigand::list<Os..., typename SecondaryPeripherals<T>::type>,
                             brigand::list<Os...>>,
          Ts...> {};

    // The entries an ISR-context contract is read from: every core's list, and the user
    // log backend even when no list names it, since listing it is only a convention.
    template<typename Backend>
    constexpr bool isCompleteType = requires { sizeof(Backend); };

    template<typename CoreLists, typename Backend>
    struct IsrContractEntries;

    template<typename... Ls, typename Backend>
    struct IsrContractEntries<brigand::list<Ls...>, Backend> {
        using Listed = brigand::append<Ls...>;
        using type = std::conditional_t<isCompleteType<Backend> && !MPL::contains<Backend, Listed>,
                                        brigand::append<Listed, brigand::list<Backend>>,
                                        Listed>;
    };

    // The image's CRC engine set (`Hook::type`, Hook = Kvasir::Crc::ImageEngines<Tag::User>; read here, where the
    // checks are instantiated, so the image's specialisation is seen) when it claims or provides a resource - a chip's
    // set over its hardware: the DMA sniffer's channel - and no core's list names it: then nothing reserves that
    // hardware. A Context without the hook (void: the host tests' fakes) has none.
    template<typename Hook, typename CoreLists>
    struct UnlistedEngineSet {
        using type = brigand::list<>;
    };

    template<typename Hook, typename... Ls>
        requires(!std::is_void_v<Hook>)
    struct UnlistedEngineSet<Hook, brigand::list<Ls...>> {
        using Set  = typename Hook::type;
        using type = std::conditional_t<ListRules::hasResources<Set>
                                          && !MPL::contains<Set, brigand::append<Ls...>>,
                                        brigand::list<Diagnostics::Pair<Set, void>>,
                                        brigand::list<>>;
    };
}}}   // namespace Kvasir::Startup::Detail

namespace Kvasir { namespace Startup { namespace Checks {
    using Diagnostics::none;
    using Diagnostics::NumberKind;
    using Diagnostics::Pair;

    enum class Role : std::uint8_t { primary, secondary };

    // What the chip and the runtime supply; StartUp.hpp passes the real ones, the host tests fakes.
    //   Traits                    the chip's Nvic::InterruptOffsetTraits<Tag::User> (begin, end, disabled, perCore)
    //   IsSecondaryCore<T>        is T a SecondaryCore
    //   SecondaryPeripherals<T>   a SecondaryCore's list (brigand::list<> for anything else)
    //   Backend                   the user log backend, whose ISR contract counts even unlisted
    //   ImageEngines              the hook Kvasir::Crc::ImageEngines<Tag::User> (its `type`: the image's CRC engine
    //                             set, which must be listed when it claims hardware); void: none
    template<typename TTraits,
             template<typename> class TIsSecondaryCore,
             template<typename> class TSecondaryPeripherals,
             typename TBackend,
             typename TImageEngines = void>
    struct Context {
        using Traits       = TTraits;
        using Backend      = TBackend;
        using ImageEngines = TImageEngines;
        template<typename T>
        using IsSecondaryCore = TIsSecondaryCore<T>;
        template<typename T>
        using SecondaryPeripherals = TSecondaryPeripherals<T>;
    };

    // ---- the rules: their texts --------------------------------------------------------------
    // a runtime hook of the wrong shape: its text names the entry, the hook and the signature (ListRules::HookShapeText)
    struct HookShape {};

    struct DuplicateEntry {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a peripheral is listed twice in one Startup list (its init steps would run twice)";
    };

    struct NotAPeripheral {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a type in the Startup list is not a peripheral (no init step, Isr, runtime hook, "
            "Provides/Claims or SecondaryCore): a driver's Config listed instead of the driver, or "
            "a "
            "typo'd alias - it would contribute nothing";
    };

    struct TwoSecondaries {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "two SecondaryCores in one Startup list: the chip has one other core";
    };

    struct SecondaryInSecondary {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a SecondaryCore inside a SecondaryCore's list: the chip has one other core, and only "
            "the primary Startup list launches it";
    };

    struct LaunchTimeoutOutside {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a LaunchTimeout in the primary Startup list is ignored: it belongs in the "
            "SecondaryCore's "
            "list whose launch it bounds";
    };

    struct TwoIsrTransforms {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "two entries in one Startup list rewrite its ISR list (IsrTransform, e.g. two "
            "IsrProfiling): only one "
            "may";
    };

    struct ClockSettingsListed {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a ClockSettings among the peripherals: its clock init never runs there (Startup's "
            "first "
            "argument is the one that does)";
    };

    struct GuardAfterReport {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a BootGuard::Guard after Boot::Report in a Startup list: the report takes the panic "
            "and fault "
            "records before the guard reads them, so the guard would judge every run healthy - "
            "list the guard "
            "before the report (Runtime::Standard does)";
    };

    struct ServiceBeforeReport {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a Health::Service<> before Boot::Report in a Startup list: the service's runtimeInit "
            "reports what the "
            "run before starved, a line that belongs after the boot line - list the service after "
            "the report (last, "
            "after the entries whose checks it judges)";
    };

    struct WatchdogAfterService {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a watchdog after Health::Service<> in a Startup list: the service's runtimeInit holds "
            "the watchdog off, "
            "and a watchdog whose own runtimeInit arms it later (an ungated SAM or STM32 one) "
            "counts through the rest "
            "of the boot - list the watchdog before the service";
    };

    struct BootReportMissing {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "a Startup list with the user log backend names no boot report: list "
            "Kvasir::Boot::Report<> (or "
            "Kvasir::Runtime::Standard<Clock>) after the backend, or Kvasir::Boot::ReportInMain "
            "where main reads "
            "the panic and fault records itself and reports them by hand - without one a panic or "
            "fault record "
            "stays unreported";
    };

    struct ChipEntryListed {
        static constexpr auto             kind = NumberKind::core;
        static constexpr std::string_view message
          = "an entry the chip package adds to every Startup list itself (Startup::ChipEntries; on "
            "RP the clock "
            "tree's Kvasir::Clocks::DegradedBoot) is listed by hand: it would run twice - drop it "
            "from the list (its "
            "options are a hook, Kvasir::Clocks::DegradedBootConfig<Kvasir::Tag::User> on RP)";
    };

    struct VectorTwice {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "two peripherals in one Startup list claim the same interrupt vector: only "
            "the first would ever run (two DmaBase instances need an interruptInstance "
            "each)";
    };

    struct VectorOutOfTable {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "a peripheral installs an Isr on an index the chip's vector table does not "
            "have (outside InterruptOffsetTraits::begin..end, or a disabled index): it "
            "would be silently absent from the table";
    };

    struct EnabledUnhandled {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "an init step enables an interrupt line for which nothing in this core's "
            "list installs an Isr: the line would fire into the unhandled-interrupt "
            "handler at its first event (a driver whose Isr alias was dropped, or an "
            "interruptEnable copied from another driver)";
    };

    struct LateDependency {
        static constexpr std::string_view message
          = "a SecondaryCore's peripheral needs a core 0 peripheral whose init has not run by the "
            "launch: list it before the SecondaryCore (ClockSync's Reference, LaunchTimeout's "
            "clock)";
    };

    struct OnBothCores {
        static constexpr std::string_view message
          = "a peripheral is listed for both cores: it belongs in exactly one Startup list";
    };

    struct RuntimeInitAfterSecondary {
        static constexpr std::string_view message
          = "nothing with a runtimeInit may follow a SecondaryCore: it would run concurrently with "
            "the other core's main - list the SecondaryCore last";
    };

    struct ImageEngineSetUnlisted {
        static constexpr std::string_view message
          = "the image's CRC engine set (Kvasir::Crc::ImageEngines<Kvasir::Tag::User>) claims "
            "hardware but is in no "
            "Startup list: list it - unlisted it reserves nothing, so a driver on its DMA channel "
            "or an image check "
            "on the sniffer is not refused and the CRCs are corrupted";
    };

    struct VectorOnBothCores {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "an interrupt is installed in both cores' vector tables: the ISR would run "
            "on both cores at once (only the core exceptions and the lines the chip lists in "
            "InterruptOffsetTraits::perCore are per core): list the peripheral on one core";
    };

    struct ContradictingIsrContract {
        static constexpr std::string_view message
          = "an entry declares both isrPriorityLevels and singleIsrPriorityLevel";
    };

    // An entry's ISR-context contract (ListRules.hpp: IsrContractHolds) against the enabled
    // interrupts, reported per (entry, index).
    struct IsrLevelUnserved {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "an interrupt is enabled at a priority level this entry has no ISR context "
            "for: an ISR at that level could preempt another ISR's log record on a ring "
            "it shares. Give the level its own context (uc_log: add it to the "
            "IsrPolicy::PerLevel Levels<...>, preferred), declare it silent if no ISR at "
            "that level ever logs (SilentLevels<...>), or use the entry's masked-record "
            "mode (IsrPolicy::MaskedRecord)";
    };

    struct IsrLevelsDiffer {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "enabled interrupts that may log use more than one priority level, and this "
            "entry keeps one ISR context for all of them: an ISR at the higher level "
            "preempts one at the lower mid-record. Configure the entry per level (uc_log: "
            "IsrPolicy::PerLevel, preferred), declare the levels whose ISRs never log as "
            "silent (SilentLevels<...>, SysTick's level 0 typically), or use its "
            "masked-record mode (IsrPolicy::MaskedRecord)";
    };

    struct IsrLevelUnknown {
        static constexpr auto             kind = NumberKind::interrupt;
        static constexpr std::string_view message
          = "an init step writes this interrupt's priority in a form the check cannot "
            "read (a run-time value, a toggle, or a literal over part of the priority "
            "bits), so its level is unknown and this entry's ISR contexts cannot be shown "
            "to cover it. Set the priority with a literal (Nvic::makeSetPriority), or use "
            "the entry's masked-record mode (uc_log: IsrPolicy::MaskedRecord)";
    };

    // ---- building offender lists -----------------------------------------------------------
    // Pair<Who, What> for every element of a list, one side fixed.
    template<typename What, typename List>
    struct EachWith;

    template<typename What, typename... Ts>
    struct EachWith<What, brigand::list<Ts...>> {
        using type = brigand::list<Pair<Ts, What>...>;
    };

    template<typename Who, typename List>
    struct WhoWithEach;

    template<typename Who, typename... Xs>
    struct WhoWithEach<Who, brigand::list<Xs...>> {
        using type = brigand::list<Pair<Who, Xs>...>;
    };

    template<typename Rule, typename Offenders>
    inline constexpr bool reported = Diagnostics::ReportAll<Rule, Offenders>::value;

    namespace Detail {
        using namespace ::Kvasir::Startup::Detail;

        template<int Core, typename List>
        using OnCore = typename EachWith<std::integral_constant<int, Core>, List>::type;

        // Pair<P, SecondaryCore> for every P a SecondaryCore's list shares with the primary one; an entry that is
        // per core by nature (perCoreEntry, Entry.hpp: IsrProfiling) may be in both.
        template<typename All>
        struct InList {
            template<typename P>
            static constexpr bool holds = MPL::contains<P, All> && !isPerCoreEntry<P>;
        };

        template<typename Ctx, typename All, typename T>
        using SharedEntriesOf = std::conditional_t<
          Ctx::template IsSecondaryCore<T>::value,
          typename EachWith<
            T,
            MPL::Filter<InList<All>, typename Ctx::template SecondaryPeripherals<T>::type>>::type,
          brigand::list<>>;

        template<typename Ctx, typename List>
        struct OnBothCoresOffenders;

        template<typename Ctx, typename... Ts>
        struct OnBothCoresOffenders<Ctx, brigand::list<Ts...>> {
            using type
              = brigand::flatten<brigand::list<SharedEntriesOf<Ctx, brigand::list<Ts...>, Ts>...>>;
        };

        // Every entry with a runtimeInit after the first SecondaryCore: it would run while the
        // other core already runs its main.
        template<typename Ctx, bool Launched, typename Out, typename... Ts>
        struct LateRuntimeInitsImpl {
            using type = Out;
        };

        template<typename Ctx, bool Launched, typename... Os, typename T, typename... Ts>
        struct LateRuntimeInitsImpl<Ctx, Launched, brigand::list<Os...>, T, Ts...>
          : LateRuntimeInitsImpl<
              Ctx,
              Launched || Ctx::template IsSecondaryCore<T>::value,
              std::conditional_t<Launched && RuntimeHook::RuntimeInit::callable<T>,
                                 brigand::list<Os..., Pair<T, void>>,
                                 brigand::list<Os...>>,
              Ts...> {};

        template<typename Ctx, typename List>
        struct LateRuntimeInits;

        template<typename Ctx, typename... Ts>
        struct LateRuntimeInits<Ctx, brigand::list<Ts...>>
          : LateRuntimeInitsImpl<Ctx, false, brigand::list<>, Ts...> {};

        // Pair<SecondaryCore, interrupt index> for each chip-wide vector both cores install.
        template<typename Ctx, typename Primary, typename T>
        using SharedVectorsOf = std::conditional_t<
          Ctx::template IsSecondaryCore<T>::value,
          typename WhoWithEach<T,
                               typename SharedVectorIndexes<
                                 typename Ctx::Traits,
                                 typename IsrsOf<Primary>::type,
                                 typename VectorsOf<typename Ctx::template SecondaryPeripherals<
                                   T>::type>::type>::type>::type,
          brigand::list<>>;

        template<typename Ctx, typename Primary, typename List>
        struct VectorsOnBothCores;

        template<typename Ctx, typename Primary, typename... Ts>
        struct VectorsOnBothCores<Ctx, Primary, brigand::list<Ts...>> {
            using type = brigand::flatten<brigand::list<SharedVectorsOf<Ctx, Primary, Ts>...>>;
        };

        struct DeclaresBothContracts {
            template<typename T>
            static constexpr bool holds
              = ListRules::declaresIsrLevels<T> && ListRules::declaresSingleIsrLevel<T>;
        };

        // The reports of one entry's contract on one group of interrupts
        // (ListRules::IsrPriorityGroupsFor), `T` the reported entry: entries without one pass.
        template<typename Prios, typename T>
        struct IsrLevelReportsFor : std::true_type {};

        template<typename Prios, typename T>
            requires(ListRules::declaresIsrLevels<T> && !ListRules::declaresSingleIsrLevel<T>)
        struct IsrLevelReportsFor<Prios, T>
          : std::bool_constant<
              reported<
                IsrLevelUnknown,
                typename WhoWithEach<T,
                                     typename ListRules::UnknownIsrLevelIndexes<Prios>::type>::type>
              && reported<IsrLevelUnserved,
                          typename WhoWithEach<T,
                                               typename ListRules::UnservedIsrIndexes<
                                                 T::isrPriorityLevels,
                                                 ListRules::silentIsrLevelsOf<T>(),
                                                 Prios>::type>::type>> {};

        template<typename Prios, typename T>
            requires(ListRules::declaresSingleIsrLevel<T> && !ListRules::declaresIsrLevels<T>)
        struct IsrLevelReportsFor<Prios, T>
          : std::bool_constant<
              reported<
                IsrLevelUnknown,
                typename WhoWithEach<T,
                                     typename ListRules::UnknownIsrLevelIndexes<Prios>::type>::type>
              && reported<IsrLevelsDiffer,
                          typename WhoWithEach<T,
                                               typename ListRules::MultiLevelIsrIndexes<
                                                 ListRules::silentIsrLevelsOf<T>(),
                                                 Prios>::type>::type>> {};

        template<typename T, typename Groups>
        struct IsrContractReportsOn;

        template<typename T, typename... Gs>
        struct IsrContractReportsOn<T, brigand::list<Gs...>>
          : std::bool_constant<(IsrLevelReportsFor<Gs, T>::value && ...)> {};

        template<typename Entries, typename CoreLists>
        struct IsrContractReports;

        template<typename... Ts, typename CoreLists>
        struct IsrContractReports<brigand::list<Ts...>, CoreLists>
          : std::bool_constant<(
              IsrContractReportsOn<
                Ts,
                typename ListRules::IsrPriorityGroupsFor<Ts, CoreLists>::type>::value
              && ...)> {};
    }   // namespace Detail

    namespace Detail {
        // The rules only one role has (instantiated for that role alone: a report fires when named).
        template<typename Ctx, Role R, int Core, typename Entries>
        struct RoleRules;

        template<typename Ctx, int Core, typename Entries>
        struct RoleRules<Ctx, Role::primary, Core, Entries>
          : std::bool_constant<
              reported<
                TwoSecondaries,
                OnCore<Core, ListRules::ExtraSecondaries<Ctx::template IsSecondaryCore, Entries>>>
              && reported<LaunchTimeoutOutside, OnCore<Core, ListRules::LaunchTimeouts<Entries>>>
              && reported<
                BootReportMissing,
                OnCore<Core, ListRules::UnreportedBackends<typename Ctx::Backend, Entries>>>> {};

        template<typename Ctx, int Core, typename Entries>
        struct RoleRules<Ctx, Role::secondary, Core, Entries>
          : std::bool_constant<reported<
              SecondaryInSecondary,
              OnCore<Core, ListRules::Secondaries<Ctx::template IsSecondaryCore, Entries>>>> {};
    }   // namespace Detail

    // The per-core lists: Primary, then each SecondaryCore's list.
    template<typename Ctx, typename Primary, typename... Ts>
    using CoreLists = typename Startup::Detail::CoreListsImpl<Ctx::template IsSecondaryCore,
                                                              Ctx::template SecondaryPeripherals,
                                                              brigand::list<Primary>,
                                                              Ts...>::type;

    // ---- one core's list ------------------------------------------------------------------------
    // Where: the type the vector reports name (the Startup or the SecondaryCore); Entries: the list
    // as written; WithDispatchers: plus SharedIsr's generated dispatchers.
    template<typename Ctx,
             Role R,
             int  Core,
             typename Where,
             typename Entries,
             typename WithDispatchers>
    struct CheckList {
        using Isrs    = typename Startup::Detail::IsrsOf<WithDispatchers>::type;
        using Enabled = typename ListRules::EnabledInterruptsIn<WithDispatchers>::type;

        static constexpr bool shapeReported
          = reported<HookShape, typename EachWith<void, ListRules::WrongHookShapes<Entries>>::type>
         && reported<DuplicateEntry, Detail::OnCore<Core, ListRules::DuplicateEntries<Entries>>>
         && reported<
              NotAPeripheral,
              Detail::OnCore<Core,
                             ListRules::NotPeripherals<Ctx::template IsSecondaryCore, Entries>>>
         && Detail::RoleRules<Ctx, R, Core, Entries>::value
         && reported<ClockSettingsListed,
                     Detail::OnCore<Core, ListRules::ClockSettingsEntries<Entries>>>
         && reported<GuardAfterReport, Detail::OnCore<Core, ListRules::GuardsAfterReport<Entries>>>
         && reported<ServiceBeforeReport,
                     Detail::OnCore<Core, ListRules::ServicesBeforeReport<Entries>>>
         && reported<WatchdogAfterService,
                     Detail::OnCore<Core, ListRules::WatchdogsAfterService<Entries>>>
         && reported<TwoIsrTransforms,
                     Detail::OnCore<Core, ListRules::ExtraIsrTransforms<Entries>>>;

        static constexpr bool interruptsReported
          = reported<
              VectorTwice,
              typename WhoWithEach<Where,
                                   typename ListRules::DuplicateIsrIndexes<Isrs>::type>::type>
         && reported<
              VectorOutOfTable,
              typename WhoWithEach<
                Where,
                typename ListRules::InvalidIsrIndexes<typename Ctx::Traits, Isrs>::type>::type>
         && reported<EnabledUnhandled,
                     typename WhoWithEach<
                       Where,
                       typename ListRules::UnhandledIndexes<Enabled, Isrs>::type>::type>;

        static constexpr bool value = shapeReported && interruptsReported;
    };

    // ---- the whole image, from the primary list ---------------------------------------------------
    template<typename Ctx,
             typename Where,
             typename ClockSettings,
             typename Entries,
             typename WithDispatchers>
    struct ImageChecks;

    template<typename Ctx,
             typename Where,
             typename ClockSettings,
             typename... Ps,
             typename WithDispatchers>
    struct ImageChecks<Ctx, Where, ClockSettings, brigand::list<Ps...>, WithDispatchers> {
        // Resources (Resources.hpp): what each peripheral provides and claims, checked over
        // this list and every SecondaryCore's list together. ClockSettings is in core 0's
        // list so it can provide the clock tree the drivers claim.
        using Resources
          = MPL::Expand<ResourceCheck, CoreLists<Ctx, brigand::list<ClockSettings, Ps...>, Ps...>>;

        // Every entry's ISR-context contract, the log backend's above all, against the enabled
        // interrupts of every core. Entries of a SecondaryCore's list count, and so does the
        // user backend when no list names it.
        using IsrCoreLists = CoreLists<Ctx, brigand::list<Ps...>, Ps...>;
        using IsrContractEntries =
          typename Startup::Detail::IsrContractEntries<IsrCoreLists, typename Ctx::Backend>::type;

        static constexpr bool value
          = Diagnostics::Diagnose<Resources>::value
         && reported<LateDependency,
                     ListRules::LateDependencies<Ctx::template IsSecondaryCore,
                                                 Ctx::template SecondaryPeripherals,
                                                 brigand::list<Ps...>>>
         && reported<OnBothCores,
                     typename Detail::OnBothCoresOffenders<Ctx, brigand::list<Ps...>>::type>
         && reported<RuntimeInitAfterSecondary,
                     typename Detail::LateRuntimeInits<Ctx, brigand::list<Ps...>>::type>
         && reported<ImageEngineSetUnlisted,
                     typename Startup::Detail::UnlistedEngineSet<typename Ctx::ImageEngines,
                                                                 IsrCoreLists>::type>
         && reported<
              VectorOnBothCores,
              typename Detail::VectorsOnBothCores<Ctx, WithDispatchers, brigand::list<Ps...>>::type>
         && reported<ContradictingIsrContract,
                     typename EachWith<
                       void,
                       MPL::Filter<Detail::DeclaresBothContracts, IsrContractEntries>>::type>
         && Detail::IsrContractReports<IsrContractEntries, IsrCoreLists>::value;
    };
}}}   // namespace Kvasir::Startup::Checks

namespace Kvasir { namespace Startup { namespace Diagnostics {
    // The hook-shape report names the hook and the signature it must have (ListRules::HookShapeText).
    template<typename Entry>
    struct Report<Checks::HookShape, Entry, void> {
        static constexpr bool fires = MPL::AlwaysFalse<Report>::value;
        KVASIR_STATIC_ASSERT(fires,
                             (ListRules::HookShapeText<Entry>));
    };
}}}   // namespace Kvasir::Startup::Diagnostics
