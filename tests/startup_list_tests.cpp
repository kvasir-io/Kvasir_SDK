// Tests for kvasir/StartUp/ListRules.hpp: the shape rules Startup and SecondaryCore apply to
// their peripheral lists. Compile time only, both ways, on fake peripherals; main() exists so
// ctest has something to run.
#include "kvasir/StartUp/ListRules.hpp"
#include "kvasir/StartUp/RamVectorTable.hpp"
#include "test_harness.hpp"

#include <array>
#include <type_traits>

namespace {

namespace LR = Kvasir::Startup::ListRules;

template<typename... Ts>
using list = brigand::list<Ts...>;

// ---- stand-ins -------------------------------------------------------------------------
struct WithPinInit {
    static constexpr int initStepPinConfig = 0;
};

struct WithEarlyInit {
    static constexpr int earlyInit = 0;
};

struct WithIsrAlias {
    using Isr = int;
};

struct WithIsrMember {
    static constexpr int isr = 0;
};

struct WithRuntimeInit {
    static void runtimeInit() {}
};

struct WithRuntimeInitLambda {   // the USB base's shape
    static constexpr auto runtimeInit = []() {};
};

struct WithPrimaryPrepare {
    static void primaryPrepare() {}
};

struct WithSync {
    static bool primarySync() { return true; }
};

struct WithClaims {
    using Claims = list<>;
};

struct WithProvides {
    using Provides = list<>;
};

struct WithCore {
    static constexpr unsigned startupCore = 1;
};

struct Timeout {
    static constexpr bool isLaunchTimeout = true;
};

struct Entry {   // Using<...>
    static constexpr bool isStartupEntry = true;
};

struct Config {   // a driver's Config struct, listed by mistake; nothing reads the members
    [[maybe_unused]] static constexpr unsigned clockSpeed = 150'000'000;
    [[maybe_unused]] static constexpr int      instance   = 0;
};

struct Empty {};

struct Clocks {
    static void coreClockInit() {}

    static void peripheryClockInit() {}
};

struct CoreClocksOnly {
    static void coreClockInit() {}
};

template<typename... Ps>
struct FakeSecondary {};

template<typename T>
struct IsFakeSecondary : std::false_type {};

template<typename... Ps>
struct IsFakeSecondary<FakeSecondary<Ps...>> : std::true_type {};

// ---- what is a peripheral ---------------------------------------------------------------
static_assert(LR::isPeripheral<WithPinInit>);
static_assert(LR::isPeripheral<WithEarlyInit>);
static_assert(LR::isPeripheral<WithIsrAlias>);
static_assert(LR::isPeripheral<WithIsrMember>);
static_assert(LR::isPeripheral<WithRuntimeInit>);
static_assert(LR::isPeripheral<WithRuntimeInitLambda>);
static_assert(LR::isPeripheral<WithPrimaryPrepare>);
static_assert(LR::isPeripheral<WithSync>);
static_assert(LR::isPeripheral<WithClaims>);
static_assert(LR::isPeripheral<WithProvides>);
static_assert(LR::isPeripheral<WithCore>);
static_assert(LR::isPeripheral<Timeout>);
static_assert(LR::isPeripheral<Entry>);
static_assert(!LR::isPeripheral<Config>);
static_assert(!LR::isPeripheral<Empty>);
static_assert(!LR::isPeripheral<int>);
// a SecondaryCore is not a peripheral by these members; the rule takes the predicate
static_assert(!LR::isPeripheral<FakeSecondary<WithPinInit>>);

// ---- A1: no type twice ------------------------------------------------------------------
static_assert(LR::NoDuplicateEntry<list<>>::value);
static_assert(LR::NoDuplicateEntry<list<WithPinInit>>::value);
static_assert(LR::NoDuplicateEntry<list<WithPinInit,
                                        WithIsrAlias,
                                        WithClaims>>::value);
static_assert(!LR::NoDuplicateEntry<list<WithPinInit,
                                         WithIsrAlias,
                                         WithPinInit>>::value);
static_assert(!LR::NoDuplicateEntry<list<WithPinInit,
                                         WithPinInit>>::value);

// ---- A2: every entry is a peripheral ----------------------------------------------------
static_assert(LR::AllArePeripherals<IsFakeSecondary,
                                    list<>>::value);
static_assert(LR::AllArePeripherals<IsFakeSecondary,
                                    list<WithPinInit,
                                         WithClaims,
                                         FakeSecondary<WithIsrAlias>>>::value);
static_assert(!LR::AllArePeripherals<IsFakeSecondary,
                                     list<WithPinInit,
                                          Config>>::value);
static_assert(!LR::AllArePeripherals<IsFakeSecondary,
                                     list<Empty>>::value);

// ---- A3: one other core -----------------------------------------------------------------
static_assert(LR::AtMostOneSecondary<IsFakeSecondary,
                                     list<>>::value);
static_assert(LR::AtMostOneSecondary<IsFakeSecondary,
                                     list<WithPinInit,
                                          FakeSecondary<>>>::value);
static_assert(!LR::AtMostOneSecondary<IsFakeSecondary,
                                      list<FakeSecondary<>,
                                           WithPinInit,
                                           FakeSecondary<WithClaims>>>::value);
// ... and none inside another's list
static_assert(LR::NoSecondaryIn<IsFakeSecondary,
                                list<WithPinInit,
                                     WithClaims>>::value);
static_assert(!LR::NoSecondaryIn<IsFakeSecondary,
                                 list<WithPinInit,
                                      FakeSecondary<>>>::value);

// ---- A4: a LaunchTimeout only in a SecondaryCore's list --------------------------------
static_assert(LR::NoLaunchTimeoutIn<list<WithPinInit,
                                         FakeSecondary<Timeout>>>::value);
static_assert(!LR::NoLaunchTimeoutIn<list<WithPinInit,
                                          Timeout>>::value);

// ---- A5: one ClockSettings, and not among the peripherals --------------------------------
static_assert(LR::NoClockSettingsIn<list<WithPinInit,
                                         WithClaims>>::value);
static_assert(!LR::NoClockSettingsIn<list<WithPinInit,
                                          Clocks>>::value);
static_assert(!LR::NoClockSettingsIn<list<CoreClocksOnly>>::value);

// ---- B1: every Isr index is in the chip's table ------------------------------------------
struct Traits {
    static constexpr int        begin    = -14;
    static constexpr int        end      = 52;
    static constexpr std::array disabled = {-4, -3};
};

struct TraitsNoDisabled {
    static constexpr int begin = 0;
    static constexpr int end   = 8;
};

template<int I>
struct FakeIsr {
    using IType = std::integral_constant<int, I>;
};

static_assert(LR::IsrIndexesValid<Traits,
                                  list<>>::value);
static_assert(LR::IsrIndexesValid<Traits,
                                  list<FakeIsr<-14>,
                                       FakeIsr<0>,
                                       FakeIsr<51>>>::value);
static_assert(!LR::IsrIndexesValid<Traits,
                                   list<FakeIsr<52>>>::value);
static_assert(!LR::IsrIndexesValid<Traits,
                                   list<FakeIsr<-15>>>::value);
static_assert(!LR::IsrIndexesValid<Traits,
                                   list<FakeIsr<0>,
                                        FakeIsr<-4>>>::value);
static_assert(LR::IsrIndexesValid<TraitsNoDisabled,
                                  list<FakeIsr<7>>>::value);
static_assert(!LR::IsrIndexesValid<TraitsNoDisabled,
                                   list<FakeIsr<8>>>::value);

// ---- order: a SecondaryCore's core 0 dependencies precede the launch ------------------------
template<typename T>
struct FakeSecondaryPeripherals {
    using type = list<>;
};

template<typename... Ps>
struct FakeSecondaryPeripherals<FakeSecondary<Ps...>> {
    using type = list<Ps...>;
};

struct RefClock {};

struct Other {};

struct NeedsRef {
    using Claims = list<Kvasir::Startup::ListedOn<RefClock, 0>>;
};

struct NeedsOther {
    using Claims = list<Kvasir::Startup::ListedOn<Other, 0>>;
};

template<typename List>
using Precede = LR::DependenciesPrecedeLaunch<IsFakeSecondary, FakeSecondaryPeripherals, List>;

static_assert(Precede<list<>>::value);
static_assert(Precede<list<RefClock,
                           FakeSecondary<NeedsRef>>>::value);
static_assert(Precede<list<RefClock,
                           Other,
                           FakeSecondary<NeedsRef,
                                         NeedsOther>>>::value);
// the reference after the SecondaryCore: its init has not run at launch
static_assert(!Precede<list<FakeSecondary<NeedsRef>,
                            RefClock>>::value);
static_assert(!Precede<list<RefClock,
                            FakeSecondary<NeedsRef,
                                          NeedsOther>,
                            Other>>::value);
// a reference that is not in the list at all is the resource rule's finding, not this one's
static_assert(Precede<list<FakeSecondary<NeedsRef>>>::value);
// no SecondaryCore, no rule
static_assert(Precede<list<NeedsRef,
                           RefClock>>::value);

// ---- enabled without a handler ------------------------------------------------------------
// Stand-ins for register actions; the chip layer maps the NVIC's enable writes.
template<int I>
struct EnableWrite {};

struct OtherWrite {};

}   // namespace

namespace Kvasir::Startup {
template<int I>
struct InterruptOfAction<EnableWrite<I>> {
    using type = brigand::list<std::integral_constant<int, I>>;
};
}   // namespace Kvasir::Startup

namespace {

template<int I>
struct HandlerFor {
    using Isr = list<FakeIsr<I>>;
};

// a driver that enables its line and installs its handler
struct Dma {
    using Isr                                     = list<FakeIsr<10>>;
    static constexpr auto initStepPeripheryEnable = list<OtherWrite, EnableWrite<10>>{};
};

// two lines in one enable step, one of them in the interrupt-config phase
struct TwoLines {
    using Isr                                     = list<FakeIsr<3>, FakeIsr<4>>;
    static constexpr auto initStepInterruptConfig = EnableWrite<3>{};
    static constexpr auto initStepPeripheryEnable = list<list<EnableWrite<4>>, OtherWrite>{};
};

// enables a line but installs no handler: the bug
struct EnableOnly {
    static constexpr auto initStepPeripheryEnable = list<EnableWrite<7>>{};
};

// a handler installed by another entry counts
struct Handler7 : HandlerFor<7> {};

static_assert(std::is_same_v<LR::EnabledInterruptsIn<list<Dma,
                                                          TwoLines>>::type,
                             list<std::integral_constant<int,
                                                         10>,
                                  std::integral_constant<int,
                                                         3>,
                                  std::integral_constant<int,
                                                         4>>>);
static_assert(std::is_same_v<LR::EnabledInterruptsIn<list<WithPinInit>>::type,
                             list<>>);

template<typename... Ts>
using Handled = LR::EnabledLinesHandled<typename LR::EnabledInterruptsIn<list<Ts...>>::type,
                                        list<FakeIsr<10>, FakeIsr<3>, FakeIsr<4>>>;

static_assert(Handled<>::value);
static_assert(Handled<Dma,
                      TwoLines>::value);
static_assert(!Handled<Dma,
                       EnableOnly>::value);
// ... with the handler from another entry in the same list
static_assert(LR::EnabledLinesHandled<LR::EnabledInterruptsIn<list<EnableOnly,
                                                                   Handler7>>::type,
                                      list<FakeIsr<7>>>::value);
// a handler on another line does not count
static_assert(!LR::EnabledLinesHandled<LR::EnabledInterruptsIn<list<EnableOnly>>::type,
                                       list<FakeIsr<8>>>::value);

// ---- interrupt priorities ---------------------------------------------------------------
// Stand-ins for the priority writes; the core layer maps the NVIC's IPR and the SCB's SHPR.
template<int I, int L>
struct PriorityWrite {};

// a priority write whose level the core layer cannot read (a run-time value)
template<int I>
struct UnknownWrite {};

}   // namespace

namespace Kvasir::Startup {
template<int I, int L>
struct PriorityOfAction<PriorityWrite<I, L>> {
    using type = brigand::list<IsrPriority<I, L>>;
};

template<int I>
struct PriorityOfAction<UnknownWrite<I>> {
    using type = brigand::list<IsrPriority<I, unknownIsrLevel>>;
};
}   // namespace Kvasir::Startup

namespace {

// a driver that enables its line at a level
template<int I, int L>
struct Prio {
    using Isr                                     = list<FakeIsr<I>>;
    static constexpr auto initStepInterruptConfig = list<PriorityWrite<I, L>, OtherWrite>{};
    static constexpr auto initStepPeripheryEnable = EnableWrite<I>{};
};

// enabled, priority never set: the reset value 0
template<int I>
struct Unset {
    using Isr                                     = list<FakeIsr<I>>;
    static constexpr auto initStepPeripheryEnable = EnableWrite<I>{};
};

// the priority set by one entry, the line enabled by another (a shared line)
struct PrioOnly9 {
    static constexpr auto initStepInterruptConfig = PriorityWrite<9, 2>{};
};

// a priority set by the interrupt-config step, or by the periphery-enable step, which Startup
// applies after every entry's interrupt-config step
template<int I, int L>
struct PrioAtConfig {
    static constexpr auto initStepInterruptConfig = PriorityWrite<I, L>{};
};

template<int I, int L>
struct PrioAtEnable {
    static constexpr auto initStepPeripheryEnable = PriorityWrite<I, L>{};
};

// enabled, the priority written in a form the check cannot read
template<int I>
struct UnknownPrio {
    using Isr                                     = list<FakeIsr<I>>;
    static constexpr auto initStepInterruptConfig = UnknownWrite<I>{};
    static constexpr auto initStepPeripheryEnable = EnableWrite<I>{};
};

template<int... Ls>
using Arr = std::array<int, sizeof...(Ls)>;

template<int... Ls>
constexpr Arr<Ls...> levels{Ls...};

template<int I, int L>
using IP = Kvasir::Startup::IsrPriority<I, L>;

static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<Prio<10,
                                                           3>,
                                                      Unset<4>,
                                                      Prio<5,
                                                           1>>>::type,
                             list<IP<10,
                                     3>,
                                  IP<4,
                                     0>,
                                  IP<5,
                                     1>>>);
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<PrioOnly9,
                                                      Unset<9>>>::type,
                             list<IP<9,
                                     2>>>,
              "the level may come from another entry in the list");
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<PrioOnly9>>::type,
                             list<>>,
              "a priority on a line nothing enables is not an enabled interrupt");
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<Prio<10,
                                                           3>,
                                                      PrioAtConfig<10,
                                                                   1>>>::type,
                             list<IP<10,
                                     1>>>,
              "of two writes in one phase the later entry's is the level");
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<PrioAtEnable<10,
                                                                   1>,
                                                      Prio<10,
                                                           3>>>::type,
                             list<IP<10,
                                     1>>>,
              "the periphery-enable phase runs after every interrupt-config step: its write is the "
              "level, though listed first");
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<UnknownPrio<4>,
                                                      PrioAtConfig<4,
                                                                   2>>>::type,
                             list<IP<4,
                                     2>>>,
              "a later literal write settles an unknown level");
static_assert(std::is_same_v<LR::IsrPrioritiesIn<list<>>::type,
                             list<>>);

// the contract members
struct ServesNone {};

struct Serves13 {
    static constexpr std::array<int, 2> isrPriorityLevels{1, 3};
};

struct Serves3Silent0 {
    static constexpr std::array<int, 1> isrPriorityLevels{3};
    static constexpr std::array<int, 1> silentIsrPriorityLevels{0};
};

struct SingleAny {
    static constexpr bool singleIsrPriorityLevel = true;
};

struct SingleSilent0 {
    static constexpr bool               singleIsrPriorityLevel = true;
    static constexpr std::array<int, 1> silentIsrPriorityLevels{0};
};

static_assert(!LR::declaresIsrLevels<ServesNone> && !LR::declaresSingleIsrLevel<ServesNone>);
static_assert(LR::declaresIsrLevels<Serves13> && !LR::declaresSingleIsrLevel<Serves13>);
static_assert(!LR::declaresIsrLevels<SingleAny> && LR::declaresSingleIsrLevel<SingleAny>);
static_assert(LR::silentIsrLevelsOf<ServesNone>().size() == 0);
static_assert(LR::silentIsrLevelsOf<SingleSilent0>() == Arr<0>{0});

template<typename... Ts>
using PriosOf = typename LR::IsrPrioritiesIn<list<Ts...>>::type;

// SingleIsrPriority: at most one level outside the silent ones
static_assert(LR::SingleIsrPriority<Arr<>{},
                                    PriosOf<>>::value,
              "nothing enabled");
static_assert(LR::SingleIsrPriority<Arr<>{},
                                    PriosOf<Prio<10,
                                                 3>>>::value);
static_assert(LR::SingleIsrPriority<Arr<>{},
                                    PriosOf<Prio<10,
                                                 3>,
                                            Prio<11,
                                                 3>>>::value,
              "two ISRs at one level never preempt each other");
static_assert(!LR::SingleIsrPriority<Arr<>{},
                                     PriosOf<Prio<10,
                                                  3>,
                                             Prio<11,
                                                  2>>>::value,
              "two levels conflict");
static_assert(!LR::SingleIsrPriority<Arr<>{},
                                     PriosOf<Prio<10,
                                                  3>,
                                             Unset<4>>>::value,
              "an unset priority is level 0, a second level");
static_assert(LR::SingleIsrPriority<levels<0>,
                                    PriosOf<Prio<10,
                                                 3>,
                                            Unset<4>>>::value,
              "... unless level 0 is silent");
static_assert(LR::SingleIsrPriority<levels<0,
                                           2>,
                                    PriosOf<Prio<10,
                                                 3>,
                                            Prio<11,
                                                 2>,
                                            Unset<4>>>::value);
static_assert(!LR::SingleIsrPriority<levels<0>,
                                     PriosOf<Prio<10,
                                                  3>,
                                             Prio<11,
                                                  2>,
                                             Unset<4>>>::value);
static_assert(LR::SingleIsrPriority<levels<3>,
                                    PriosOf<Prio<10,
                                                 3>,
                                            Prio<11,
                                                 3>>>::value,
              "every enabled level silent: nothing logs from an ISR");

// IsrPrioritiesServed: every enabled level served or silent
static_assert(LR::IsrPrioritiesServed<levels<1,
                                             3>,
                                      Arr<>{},
                                      PriosOf<>>::value);
static_assert(LR::IsrPrioritiesServed<levels<1,
                                             3>,
                                      Arr<>{},
                                      PriosOf<Prio<10,
                                                   3>,
                                              Prio<5,
                                                   1>>>::value);
static_assert(!LR::IsrPrioritiesServed<levels<1,
                                              3>,
                                       Arr<>{},
                                       PriosOf<Prio<10,
                                                    3>,
                                               Unset<4>>>::value,
              "level 0 neither served nor silent");
static_assert(LR::IsrPrioritiesServed<levels<1,
                                             3>,
                                      levels<0>,
                                      PriosOf<Prio<10,
                                                   3>,
                                              Unset<4>>>::value);
static_assert(!LR::IsrPrioritiesServed<levels<3>,
                                       levels<0>,
                                       PriosOf<Prio<10,
                                                    3>,
                                               Prio<5,
                                                    1>>>::value);
static_assert(LR::IsrPrioritiesServed<Arr<>{},
                                      levels<0,
                                             3>,
                                      PriosOf<Prio<10,
                                                   3>,
                                              Unset<4>>>::value,
              "silent alone covers it");

// unknown levels: IsrLevelsKnown reports them, the level rules leave them alone
template<typename... Ts>
using WithUnknown = PriosOf<Prio<10, 3>, UnknownPrio<4>, Ts...>;

static_assert(!LR::IsrLevelsKnown<WithUnknown<>>::value);
static_assert(LR::IsrLevelsKnown<PriosOf<Prio<10,
                                              3>,
                                         Unset<4>>>::value,
              "unset is level 0, known");
static_assert(LR::IsrLevelsKnown<PriosOf<>>::value);
static_assert(LR::SingleIsrPriority<Arr<>{},
                                    WithUnknown<>>::value);
static_assert(LR::IsrPrioritiesServed<levels<3>,
                                      Arr<>{},
                                      WithUnknown<>>::value);

// IsrContractHolds: one entry's contract against the lists of every core
struct PerCoreSingle {
    static constexpr bool singleIsrPriorityLevel = true;
    static constexpr bool perCoreIsrContexts     = true;
};

struct PerCoreOff {
    static constexpr bool singleIsrPriorityLevel = true;
    static constexpr bool perCoreIsrContexts     = false;
};

struct Both {
    static constexpr std::array<int, 1> isrPriorityLevels{3};
    static constexpr bool               singleIsrPriorityLevel = true;
};

static_assert(LR::declaresPerCoreIsrContexts<PerCoreSingle>);
static_assert(!LR::declaresPerCoreIsrContexts<PerCoreOff>
              && !LR::declaresPerCoreIsrContexts<SingleAny>);
static_assert(LR::declaresIsrContract<Both> && !LR::declaresIsrContract<ServesNone>);

static_assert(LR::IsrContractHolds<ServesNone,
                                   list<list<Prio<10,
                                                  3>,
                                             UnknownPrio<4>>>>::value,
              "no contract, nothing to hold");
static_assert(LR::IsrContractHolds<Both,
                                   list<list<Prio<10,
                                                  3>,
                                             Prio<5,
                                                  1>>>>::value,
              "both members: Startup's own assert, not this rule");
static_assert(LR::IsrContractHolds<Serves3Silent0,
                                   list<list<Prio<10,
                                                  3>,
                                             Unset<4>>>>::value);
static_assert(!LR::IsrContractHolds<Serves3Silent0,
                                    list<list<Prio<10,
                                                   3>>,
                                         list<Prio<5,
                                                   1>>>>::value,
              "the second core's level 1 is not served");
static_assert(!LR::IsrContractHolds<Serves3Silent0,
                                    list<list<Prio<10,
                                                   3>,
                                              UnknownPrio<4>>>>::value,
              "an unknown level fails the contract");
static_assert(!LR::IsrContractHolds<Serves13,
                                    list<list<Prio<10,
                                                   3>,
                                              UnknownPrio<4>>>>::value);
static_assert(LR::IsrContractHolds<SingleSilent0,
                                   list<list<Prio<10,
                                                  3>,
                                             Unset<4>>>>::value);
static_assert(!LR::IsrContractHolds<SingleAny,
                                    list<list<Prio<10,
                                                   3>>,
                                         list<Prio<5,
                                                   1>>>>::value,
              "one set of contexts for both cores: levels 3 and 1 conflict");
static_assert(!LR::IsrContractHolds<PerCoreOff,
                                    list<list<Prio<10,
                                                   3>>,
                                         list<Prio<5,
                                                   1>>>>::value);
static_assert(LR::IsrContractHolds<PerCoreSingle,
                                   list<list<Prio<10,
                                                  3>>,
                                        list<Prio<5,
                                                  1>>>>::value,
              "per-core contexts: each core has one level");
static_assert(!LR::IsrContractHolds<PerCoreSingle,
                                    list<list<Prio<10,
                                                   3>,
                                              Prio<5,
                                                   1>>,
                                         list<>>>::value,
              "per-core contexts still need one level per core");
static_assert(std::is_same_v<LR::IsrPriorityGroupsFor<PerCoreSingle,
                                                      list<list<Prio<10,
                                                                     3>>,
                                                           list<Prio<5,
                                                                     1>>>>::type,
                             list<list<IP<10,
                                          3>>,
                                  list<IP<5,
                                          1>>>>);
static_assert(std::is_same_v<LR::IsrPriorityGroupsFor<SingleAny,
                                                      list<list<Prio<10,
                                                                     3>>,
                                                           list<Prio<5,
                                                                     1>>>>::type,
                             list<list<IP<10,
                                          3>,
                                       IP<5,
                                          1>>>>);

// ---- the offending indexes, for the reports ------------------------------------------------
template<int... Is>
using Idx = list<std::integral_constant<int, Is>...>;

static_assert(std::is_same_v<LR::UnservedIsrIndexes<levels<3>,
                                                    levels<0>,
                                                    PriosOf<Prio<10,
                                                                 3>,
                                                            Prio<5,
                                                                 1>,
                                                            Unset<4>,
                                                            Prio<6,
                                                                 2>>>::type,
                             Idx<5,
                                 6>>);
static_assert(std::is_same_v<LR::UnservedIsrIndexes<levels<3>,
                                                    Arr<>{},
                                                    PriosOf<Prio<10,
                                                                 3>>>::type,
                             Idx<>>);
static_assert(std::is_same_v<LR::MultiLevelIsrIndexes<levels<0>,
                                                      PriosOf<Prio<10,
                                                                   3>,
                                                              Unset<4>,
                                                              Prio<6,
                                                                   2>>>::type,
                             Idx<10,
                                 6>>,
              "every non-silent index is part of the conflict");
static_assert(std::is_same_v<LR::MultiLevelIsrIndexes<Arr<>{},
                                                      PriosOf<Prio<10,
                                                                   3>,
                                                              Prio<11,
                                                                   3>>>::type,
                             Idx<>>);
static_assert(std::is_same_v<LR::MultiLevelIsrIndexes<levels<0>,
                                                      PriosOf<Prio<10,
                                                                   3>,
                                                              Unset<4>>>::type,
                             Idx<>>);
static_assert(std::is_same_v<LR::UnknownIsrLevelIndexes<WithUnknown<Prio<6,
                                                                         2>>>::type,
                             Idx<4>>);
static_assert(std::is_same_v<LR::UnservedIsrIndexes<levels<3>,
                                                    Arr<>{},
                                                    WithUnknown<>>::type,
                             Idx<>>,
              "an unknown level is reported as unknown, not as unserved");
static_assert(std::is_same_v<LR::MultiLevelIsrIndexes<Arr<>{},
                                                      WithUnknown<Prio<6,
                                                                       2>>>::type,
                             Idx<10,
                                 6>>,
              "nor as part of a level conflict");

// ---- RamVectorTable's size against the chip's vector count -----------------------------
struct Chip52 {   // the RP2350: 16 core exceptions + 52 IRQs = 68 vectors
    static constexpr int end = 52;
};

struct Chip48 {
    static constexpr int end = 48;
};

static_assert(sizeof(Kvasir::Startup::RamVectorTable<128,
                                                     Chip52>)
              > 0);
static_assert(sizeof(Kvasir::Startup::RamVectorTable<64,
                                                     Chip48>)
                > 0,
              "64 vectors exactly fit");

static_assert(std::is_same_v<LR::DuplicateIsrIndexes<list<FakeIsr<1>,
                                                          FakeIsr<2>>>::type,
                             Idx<>>);
static_assert(std::is_same_v<LR::DuplicateIsrIndexes<list<FakeIsr<1>,
                                                          FakeIsr<2>,
                                                          FakeIsr<1>>>::type,
                             Idx<1,
                                 1>>);
static_assert(std::is_same_v<LR::InvalidIsrIndexes<Traits,
                                                   list<FakeIsr<0>,
                                                        FakeIsr<-4>,
                                                        FakeIsr<52>>>::type,
                             Idx<-4,
                                 52>>);
static_assert(std::is_same_v<LR::UnhandledIndexes<Idx<7,
                                                      3>,
                                                  list<FakeIsr<3>>>::type,
                             Idx<7>>);
static_assert(std::is_same_v<LR::UnhandledIndexes<Idx<>,
                                                  list<>>::type,
                             Idx<>>);

}   // namespace

int main() {
    Kvasir::Test::test("startup_list");
    return Kvasir::Test::report();
}
