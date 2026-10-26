// Tests for kvasir/StartUp/ListRules.hpp: the shape rules Startup and SecondaryCore apply to
// their peripheral lists. Compile time only, both ways, on fake peripherals; main() exists so
// ctest has something to run.
#include "kvasir/Health/WatchdogFront.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/StartUp/Checks.hpp"
#include "kvasir/StartUp/ListRules.hpp"
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
    [[maybe_unused]] static constexpr int isr = 0;
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

// ---- the runtime hooks' shapes ------------------------------------------------------------
// wrong shapes: declared, never callable as Startup calls them
struct BoolRuntimeInit {
    static bool runtimeInit() { return true; }
};

struct BoolPreEnable {
    static bool preEnableRuntimeInit() { return true; }
};

struct VoidPrimarySync {
    static void primarySync() {}
};

struct IntSecondarySync {
    static int secondarySync() { return 1; }
};

struct NonStaticRuntimeInit {
    void runtimeInit() {}
};

struct RuntimeInitWithArg {
    static void runtimeInit(int) {}
};

// right shapes that were not hooks before
struct PreMemoryInitOnly {
    static void preMemoryInit() {}
};

struct TemplateRuntimeInit {
    template<typename = void>
    static void runtimeInit() {}
};

namespace RH = Kvasir::Startup::Detail::RuntimeHook;   // Entry.hpp

static_assert(RH::RuntimeInit::callable<WithRuntimeInit>
              && RH::RuntimeInit::declared<WithRuntimeInit>);
static_assert(RH::RuntimeInit::callable<WithRuntimeInitLambda>
              && RH::RuntimeInit::shapeOk<WithRuntimeInitLambda>());
static_assert(RH::RuntimeInit::callable<TemplateRuntimeInit>
              && RH::RuntimeInit::shapeOk<TemplateRuntimeInit>());
static_assert(!RH::RuntimeInit::callable<NonStaticRuntimeInit>
              && RH::RuntimeInit::declared<NonStaticRuntimeInit>);
static_assert(!RH::RuntimeInit::callable<RuntimeInitWithArg>
              && RH::RuntimeInit::declared<RuntimeInitWithArg>);
static_assert(!RH::RuntimeInit::declared<Empty> && RH::RuntimeInit::shapeOk<Empty>());
static_assert(RH::PrimarySync::shapeOk<WithSync>() && !RH::PrimarySync::shapeOk<VoidPrimarySync>());

template<typename T>
constexpr bool hooksHold = LR::HookShapesHold<list<T>>::value;

template<typename T>
constexpr bool offenderIs = std::is_same_v<LR::WrongHookShapes<list<WithPinInit, T>>, list<T>>;

static_assert(!hooksHold<BoolRuntimeInit> && offenderIs<BoolRuntimeInit>);
static_assert(!hooksHold<BoolPreEnable> && offenderIs<BoolPreEnable>);
static_assert(!hooksHold<VoidPrimarySync> && offenderIs<VoidPrimarySync>);
static_assert(!hooksHold<IntSecondarySync> && offenderIs<IntSecondarySync>);
static_assert(!hooksHold<NonStaticRuntimeInit> && offenderIs<NonStaticRuntimeInit>);
static_assert(!hooksHold<RuntimeInitWithArg> && offenderIs<RuntimeInitWithArg>);
static_assert(LR::HookShapesHold<list<WithPinInit,
                                      WithEarlyInit,
                                      WithRuntimeInit,
                                      WithRuntimeInitLambda,
                                      WithPrimaryPrepare,
                                      WithSync,
                                      PreMemoryInitOnly,
                                      TemplateRuntimeInit,
                                      Config,
                                      Empty>>::value);
static_assert(std::is_same_v<LR::Detail::FirstWrongHook<VoidPrimarySync,
                                                        RH::All>::type,
                             RH::PrimarySync>);
// a declared hook makes an entry, whatever its shape: the shape rule then names it, not "not a peripheral"
static_assert(LR::isPeripheral<PreMemoryInitOnly> && LR::isPeripheral<NonStaticRuntimeInit>
              && LR::isPeripheral<BoolRuntimeInit>);

// ---- what is a peripheral ---------------------------------------------------------------
static_assert(LR::isPeripheral<WithPinInit>);
static_assert(LR::isPeripheral<WithEarlyInit>);
static_assert(LR::isPeripheral<WithIsrAlias>);
// `using Isr = ...` is the one ISR spelling: a member named isr makes no entry
static_assert(!LR::isPeripheral<WithIsrMember>);
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

// the offender list, whose every entry the compile error names
static_assert(std::is_same_v<LR::DuplicateEntries<list<WithIsrAlias,
                                                       WithPinInit,
                                                       WithClaims,
                                                       WithPinInit>>,
                             list<WithPinInit>>);
static_assert(std::is_same_v<LR::DuplicateEntries<list<WithPinInit,
                                                       WithClaims>>,
                             list<>>);

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

static_assert(std::is_same_v<brigand::front<LR::NotPeripherals<IsFakeSecondary,
                                                               list<WithPinInit,
                                                                    Config,
                                                                    Empty>>>,
                             Config>);

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

static_assert(std::is_same_v<LR::LaunchTimeouts<list<WithPinInit,
                                                     Timeout>>,
                             list<Timeout>>);

// ---- A5: one ClockSettings, and not among the peripherals --------------------------------
static_assert(LR::NoClockSettingsIn<list<WithPinInit,
                                         WithClaims>>::value);
static_assert(!LR::NoClockSettingsIn<list<WithPinInit,
                                          Clocks>>::value);
static_assert(!LR::NoClockSettingsIn<list<CoreClocksOnly>>::value);
static_assert(std::is_same_v<LR::ClockSettingsEntries<list<WithPinInit,
                                                           Clocks>>,
                             list<Clocks>>);

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

// ---- the shape rules' offender lists (each rule holds when its list is empty) --------------
namespace Dg = Kvasir::Startup::Diagnostics;
// two duplicates of one type give one entry; every duplicated type, first occurrence order
static_assert(std::is_same_v<LR::DuplicateEntries<list<WithPinInit,
                                                       WithClaims,
                                                       WithPinInit,
                                                       WithPinInit,
                                                       WithClaims>>,
                             list<WithPinInit,
                                  WithClaims>>);
static_assert(std::is_same_v<LR::DuplicateEntries<list<WithPinInit,
                                                       WithClaims>>,
                             list<>>);
static_assert(std::is_same_v<LR::NotPeripherals<IsFakeSecondary,
                                                list<WithPinInit,
                                                     Config,
                                                     FakeSecondary<>,
                                                     Empty>>,
                             list<Config,
                                  Empty>>);
static_assert(std::is_same_v<LR::NotPeripherals<IsFakeSecondary,
                                                list<WithPinInit,
                                                     FakeSecondary<>>>,
                             list<>>);
static_assert(std::is_same_v<LR::Secondaries<IsFakeSecondary,
                                             list<FakeSecondary<>,
                                                  WithPinInit,
                                                  FakeSecondary<Timeout>>>,
                             list<FakeSecondary<>,
                                  FakeSecondary<Timeout>>>);
static_assert(std::is_same_v<LR::ExtraSecondaries<IsFakeSecondary,
                                                  list<FakeSecondary<>,
                                                       WithPinInit,
                                                       FakeSecondary<Timeout>>>,
                             list<FakeSecondary<Timeout>>>);
static_assert(std::is_same_v<LR::ExtraSecondaries<IsFakeSecondary,
                                                  list<WithPinInit,
                                                       FakeSecondary<>>>,
                             list<>>);
static_assert(std::is_same_v<LR::ExtraSecondaries<IsFakeSecondary,
                                                  list<>>,
                             list<>>);
static_assert(std::is_same_v<LR::LaunchTimeouts<list<WithPinInit,
                                                     Timeout>>,
                             list<Timeout>>);
static_assert(std::is_same_v<LR::LaunchTimeouts<list<WithPinInit>>,
                             list<>>);
static_assert(std::is_same_v<LR::ClockSettingsEntries<list<Clocks,
                                                           WithPinInit,
                                                           CoreClocksOnly>>,
                             list<Clocks,
                                  CoreClocksOnly>>);
static_assert(std::is_same_v<LR::ClockSettingsEntries<list<WithPinInit>>,
                             list<>>);
static_assert(std::is_same_v<LR::WrongHookShapes<list<WithRuntimeInit,
                                                      BoolRuntimeInit,
                                                      VoidPrimarySync>>,
                             list<BoolRuntimeInit,
                                  VoidPrimarySync>>);
static_assert(std::is_same_v<LR::WrongHookShapes<list<WithRuntimeInit,
                                                      WithSync>>,
                             list<>>);
// a late dependency names the dependency and the SecondaryCore whose list needs it
static_assert(std::is_same_v<LR::LateDependencies<IsFakeSecondary,
                                                  FakeSecondaryPeripherals,
                                                  list<RefClock,
                                                       FakeSecondary<NeedsRef,
                                                                     NeedsOther>,
                                                       Other>>,
                             list<Dg::Pair<Other,
                                           FakeSecondary<NeedsRef,
                                                         NeedsOther>>>>);
static_assert(std::is_same_v<LR::LateDependencies<IsFakeSecondary,
                                                  FakeSecondaryPeripherals,
                                                  list<FakeSecondary<NeedsRef>>>,
                             list<>>);

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
static_assert(LR::silentIsrLevelsOf<ServesNone>().empty());
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

// A fan-out entry with perCoreIsrContexts is held per core. The fake has what a two-core fan-out log backend hands
// on (no logging type here): one sink's single-level contract with SysTick silent, the sink's per-core contexts, and
// the record hooks. Core 0 logs from level 3, core 1 from level 1: per core one level each.
struct FanOutPerCore {
    static constexpr bool               singleIsrPriorityLevel = true;
    static constexpr std::array<int, 1> silentIsrPriorityLevels{0};
    static constexpr bool               perCoreIsrContexts = true;

    static void write() {}

    static void initTransfer() {}

    static void finalizeTransfer() {}
};

// the same fan-out behind one set of contexts for both cores (no perCoreIsrContexts handed on)
struct FanOutShared {
    static constexpr bool               singleIsrPriorityLevel = true;
    static constexpr std::array<int, 1> silentIsrPriorityLevels{0};

    static void write() {}
};

static_assert(LR::declaresPerCoreIsrContexts<FanOutPerCore>
              && !LR::declaresPerCoreIsrContexts<FanOutShared>);
static_assert(LR::IsrContractHolds<FanOutPerCore,
                                   list<list<Prio<10,
                                                  3>,
                                             Prio<15,
                                                  0>>,
                                        list<Prio<5,
                                                  1>,
                                             Prio<15,
                                                  0>>>>::value,
              "a per-core fan-out: core 0 at 3, core 1 at 1, SysTick silent on both");
static_assert(!LR::IsrContractHolds<FanOutShared,
                                    list<list<Prio<10,
                                                   3>,
                                              Prio<15,
                                                   0>>,
                                         list<Prio<5,
                                                   1>,
                                              Prio<15,
                                                   0>>>>::value,
              "the same lists against shared contexts: levels 3 and 1 conflict");
static_assert(!LR::IsrContractHolds<FanOutPerCore,
                                    list<list<Prio<10,
                                                   3>,
                                              Prio<11,
                                                   2>>,
                                         list<Prio<5,
                                                   1>>>>::value,
              "per core still one level: core 0's 3 and 2 conflict");

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

// ---- Entry.hpp: the phase table ----------------------------------------------------------
namespace SD = Kvasir::Startup::Detail;

template<int N>
struct Act {};   // a stand-in action

struct PinsOne {
    static constexpr Act<1> initStepPinConfig{};
};

struct PinsList {
    static constexpr list<Act<2>, Act<3>> initStepPinConfig{};
};

struct PinsNested {
    static constexpr list<Act<4>, list<Act<5>, list<Act<6>>>> initStepPinConfig{};
};

struct EveryPhase {
    static constexpr Act<10> earlyInit{};
    static constexpr Act<11> powerClockEnable{};
    static constexpr Act<12> initStepPinConfig{};
    static constexpr Act<13> initStepPeripheryConfig{};
    static constexpr Act<14> initStepInterruptConfig{};
    static constexpr Act<15> initStepPeripheryEnable{};
};

struct OnlyPreMemoryInit {
    static void preMemoryInit() {}
};

static_assert(SD::Phase::Pins::has<WithPinInit> && !SD::Phase::Early::has<WithPinInit>);
static_assert(SD::Phase::Early::has<WithEarlyInit> && !SD::Phase::Pins::has<WithEarlyInit>);
static_assert(SD::Phase::Early::has<EveryPhase> && SD::Phase::PowerClock::has<EveryPhase>
              && SD::Phase::Pins::has<EveryPhase> && SD::Phase::Periphery::has<EveryPhase>
              && SD::Phase::Interrupts::has<EveryPhase> && SD::Phase::Enable::has<EveryPhase>);
static_assert(!SD::Phase::Interrupts::has<Empty> && !SD::Phase::Enable::has<WithPinInit>);
// each phase reads its own member
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::Early>::type,
                             list<Act<10>>>);
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::PowerClock>::type,
                             list<Act<11>>>);
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::Pins>::type,
                             list<Act<12>>>);
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::Periphery>::type,
                             list<Act<13>>>);
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::Interrupts>::type,
                             list<Act<14>>>);
static_assert(std::is_same_v<SD::PhaseActionsOf<EveryPhase,
                                                SD::Phase::Enable>::type,
                             list<Act<15>>>);
// no member: empty
static_assert(std::is_same_v<SD::PhaseActionsOf<Empty,
                                                SD::Phase::Pins>::type,
                             list<>>);
// a single action, a list, a nested list and none: per entry in list order, flattened
static_assert(std::is_same_v<SD::PhaseActions<SD::Phase::Pins,
                                              PinsOne,
                                              PinsList,
                                              Empty,
                                              PinsNested>,
                             list<Act<1>,
                                  Act<2>,
                                  Act<3>,
                                  Act<4>,
                                  Act<5>,
                                  Act<6>>>);
static_assert(std::is_same_v<SD::PhaseActions<SD::Phase::Pins>,
                             list<>>);
static_assert(std::is_same_v<SD::PhaseActionsInT<SD::Phase::Pins,
                                                 list<PinsList,
                                                      PinsOne>>::type,
                             list<Act<2>,
                                  Act<3>,
                                  Act<1>>>);
// hasInitSteps truth table
static_assert(LR::hasInitSteps<WithPinInit> && LR::hasInitSteps<WithEarlyInit>
              && LR::hasInitSteps<EveryPhase>);
static_assert(!LR::hasInitSteps<Empty> && !LR::hasInitSteps<WithRuntimeInit>
              && !LR::hasInitSteps<WithIsrAlias>);
// an entry with only preMemoryInit is a peripheral (a declared hook makes one)
static_assert(LR::isPeripheral<OnlyPreMemoryInit> && !LR::hasInitSteps<OnlyPreMemoryInit>);

// the bool hooks: true without the hook
struct SyncFalse {
    static bool secondarySync() { return false; }
};

static_assert(RH::SecondarySync::callable<SyncFalse> && !RH::SecondarySync::callable<Empty>);

// ---- group entries (Entry.hpp) ---------------------------------------------------------
int callLog[8]{};
int calls = 0;

template<int N>
struct InitN {
    static void runtimeInit() { callLog[calls++] = N; }
};

struct SyncTrue {
    static bool secondarySync() { return true; }
};

void extendFn() {}

struct WithExtends {
    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &extendFn>;
};

struct Group2 {
    using StartupEntries = list<PinsOne, InitN<1>>;
};

struct GroupNested {   // a group in a group, with plain members around it
    using StartupEntries = list<PinsList, Group2, InitN<2>, WithExtends>;
};

struct GroupEmpty {
    using StartupEntries = list<>;
};

struct GroupSync {
    using StartupEntries = list<SyncTrue, SyncFalse>;
};

struct GroupResources {
    using StartupEntries = list<WithProvides, WithClaims>;
};

static_assert(SD::isStartupGroup<Group2> && SD::isStartupGroup<GroupEmpty>
              && !SD::isStartupGroup<PinsOne>);
// a list without a group is itself, type for type (Startup's types do not change: byte-identical images)
static_assert(std::is_same_v<SD::Entries<PinsOne,
                                         Empty,
                                         InitN<1>>,
                             list<PinsOne,
                                  Empty,
                                  InitN<1>>>);
static_assert(std::is_same_v<SD::Entries<>,
                             list<>>);
// a group is its members in its place, nested groups too, an empty group nothing
static_assert(std::is_same_v<SD::Entries<Empty,
                                         Group2,
                                         InitN<3>>,
                             list<Empty,
                                  PinsOne,
                                  InitN<1>,
                                  InitN<3>>>);
static_assert(std::is_same_v<SD::Entries<GroupNested,
                                         GroupEmpty>,
                             list<PinsList,
                                  PinsOne,
                                  InitN<1>,
                                  InitN<2>,
                                  WithExtends>>);
static_assert(std::is_same_v<SD::EntriesIn<list<Group2>>,
                             list<PinsOne,
                                  InitN<1>>>);
// read once: every table reads the list through Entries<...> - phases, hooks (declared, shape, call), Extends,
// Provides/Claims, ISRs are the members'; a group read as one entry has none of its own
static_assert(std::is_same_v<SD::PhaseActionsInT<SD::Phase::Pins,
                                                 SD::Entries<GroupNested>>::type,
                             list<Act<2>,
                                  Act<3>,
                                  Act<1>>>);
static_assert(std::is_same_v<SD::PhaseActionsInT<SD::Phase::Pins,
                                                 SD::Entries<PinsNested,
                                                             Group2>>::type,
                             list<Act<4>,
                                  Act<5>,
                                  Act<6>,
                                  Act<1>>>);
static_assert(!SD::hasPhaseMember<Group2> && !SD::hasPhaseMember<GroupNested>
              && !LR::hasInitSteps<GroupNested>);
static_assert(!RH::RuntimeInit::declared<Group2> && !RH::RuntimeInit::callable<Group2>
              && !RH::SecondarySync::callable<GroupSync>);

struct BadShape {
    static int runtimeInit() { return 0; }
};

struct GroupBadShape {
    using StartupEntries = list<Empty, BadShape>;
};

static_assert(std::is_same_v<LR::WrongHookShapes<SD::Entries<GroupBadShape>>,
                             list<BadShape>>);
static_assert(std::is_same_v<Kvasir::MPL::Expand<SD::AllExtends,
                                                 SD::Entries<GroupNested>>,
                             SD::ExtendsOf<WithExtends>::type>
              && std::is_same_v<SD::ExtendsOf<GroupNested>::type,
                                list<>>);
static_assert(std::is_same_v<SD::ExplicitProvides<GroupResources>::type,
                             list<>>
              && std::is_same_v<SD::ClaimsOf<GroupResources>::type,
                                list<>>);

struct IsrA {
    using Isr = list<Act<7>>;
};

struct GroupIsr {
    using StartupEntries = list<Empty, IsrA>;
};

static_assert(std::is_same_v<SD::IsrsOf<SD::Entries<GroupIsr>>::type,
                             SD::IsrsOf<list<IsrA>>::type>
              && std::is_same_v<SD::IsrsOf<list<GroupIsr>>::type,
                                list<>>);

// ---- the boot report rules -------------------------------------------------------------
struct Backend {
    static void runtimeInit() {}
};

struct Report {
    static constexpr bool isBootReport = true;

    static void runtimeInit() {}
};

struct InMain {
    static constexpr bool isStartupEntry   = true;
    static constexpr bool bootReportInMain = true;
};

struct Guard {
    static constexpr bool isBootGuard = true;

    static void runtimeInit() {}
};

struct StandardLike {   // Runtime::Standard's shape
    using StartupEntries = list<WithEarlyInit, WithRuntimeInit, Guard, Report>;
};

static_assert(LR::isBootReport<Report> && !LR::isBootReport<InMain>
              && LR::isBootReportInMain<InMain>);
static_assert(LR::isBootGuard<Guard> && !LR::isBootGuard<Report> && LR::isPeripheral<InMain>);
// a guard before the report, or either alone: holds; the guard after the report: named
static_assert(LR::GuardBeforeReport<list<Backend,
                                         Guard,
                                         Report>>::value);
static_assert(LR::GuardBeforeReport<list<Guard>>::value
              && LR::GuardBeforeReport<list<Report>>::value);
static_assert(!LR::GuardBeforeReport<list<Backend,
                                          Report,
                                          Empty,
                                          Guard>>::value);
static_assert(std::is_same_v<LR::GuardsAfterReport<list<Report,
                                                        Guard>>,
                             list<Guard>>);
static_assert(LR::GuardBeforeReport<SD::Entries<Backend,
                                                StandardLike>>::value);

// a Health::Service<> after the report, or without one: holds; before it: named (a group's report counts)
struct ServiceLike {
    static constexpr bool isHealthService = true;

    static void runtimeInit() {}
};

static_assert(LR::isHealthService<ServiceLike> && !LR::isHealthService<Report>);
static_assert(LR::HealthServiceAfterReport<list<Backend,
                                                Report,
                                                ServiceLike>>::value
              && LR::HealthServiceAfterReport<list<ServiceLike>>::value
              && LR::HealthServiceAfterReport<list<ServiceLike,
                                                   InMain>>::value);
static_assert(std::is_same_v<LR::ServicesBeforeReport<list<ServiceLike,
                                                           Empty,
                                                           Report>>,
                             list<ServiceLike>>);
static_assert(!LR::HealthServiceAfterReport<SD::Entries<ServiceLike,
                                                        StandardLike>>::value);

// a chip watchdog (the front's isWatchdog, inherited) before the service, or either alone: holds; after it: named
struct DogLike : Kvasir::Health::WatchdogFront<DogLike, false> {
    static void runtimeInit() {}

private:
    friend struct Kvasir::Health::WatchdogFront<DogLike, false>;

    static void armNow() {}

    static void feedNow() {}
};

static_assert(LR::isWatchdog<DogLike> && !LR::isWatchdog<ServiceLike>);
static_assert(LR::WatchdogBeforeHealthService<list<DogLike,
                                                   ServiceLike>>::value
              && LR::WatchdogBeforeHealthService<list<DogLike>>::value
              && LR::WatchdogBeforeHealthService<list<ServiceLike>>::value);
static_assert(std::is_same_v<LR::WatchdogsAfterService<list<Report,
                                                            ServiceLike,
                                                            Empty,
                                                            DogLike>>,
                             list<DogLike>>);

// a gated watchdog has no runtimeInit (the supervisor arms it at its first turn): after the service it holds
struct GatedDogLike : Kvasir::Health::WatchdogFront<GatedDogLike, true> {
private:
    friend struct Kvasir::Health::WatchdogFront<GatedDogLike, true>;

    static void armNow() {}

    static void feedNow() {}
};

static_assert(LR::isWatchdog<GatedDogLike> && LR::isGatedWatchdog<GatedDogLike>
              && !LR::isGatedWatchdog<DogLike>);
static_assert(LR::WatchdogBeforeHealthService<list<ServiceLike,
                                                   GatedDogLike>>::value);
// the backend without a report: named; with the report, the marker, or the bundle: holds; no backend: holds
static_assert(!LR::BootReportListed<Backend,
                                    list<Backend,
                                         WithPinInit>>::value);
static_assert(std::is_same_v<LR::UnreportedBackends<Backend,
                                                    list<Backend>>,
                             list<Backend>>);
static_assert(LR::BootReportListed<Backend,
                                   list<Backend,
                                        Report>>::value);
static_assert(LR::BootReportListed<Backend,
                                   list<InMain,
                                        Backend>>::value);
static_assert(LR::BootReportListed<Backend,
                                   SD::Entries<Backend,
                                               StandardLike>>::value);
static_assert(!LR::BootReportListed<Backend,
                                    list<Backend,
                                         StandardLike>>::value);   // read through Entries
static_assert(LR::BootReportListed<Backend,
                                   list<WithPinInit>>::value);

// the chip's own entries (Startup::ChipEntries, Entry.hpp): one group in front of the listed entries, empty without a
// specialisation (the list's Entries unchanged); a chip entry listed by hand is named (ChipEntryNotListed)
struct ChipPiece {
    static constexpr bool chipEntry = true;

    static void postMemoryInit() {}
};

struct FakeClockSettings {
    static void coreClockInit() {}
};

struct ChipSet {   // the `type` of a chip's specialisation of Startup::ChipEntries
    template<typename ClockSettings>
    using For = list<ChipPiece>;
};

static_assert(SD::isChipEntry<ChipPiece> && !SD::isChipEntry<WithPinInit>
              && LR::isPeripheral<ChipPiece>);
static_assert(
  std::is_same_v<SD::Entries<SD::ChipEntriesGroup<FakeClockSettings,
                                                  Kvasir::Startup::ChipEntries<void>::type>,
                             WithPinInit>,
                 list<WithPinInit>>);
static_assert(std::is_same_v<SD::Entries<SD::ChipEntriesGroup<FakeClockSettings,
                                                              ChipSet>,
                                         WithPinInit>,
                             list<ChipPiece,
                                  WithPinInit>>);
static_assert(std::is_same_v<LR::ListedChipEntries<list<WithPinInit,
                                                        ChipPiece>>,
                             list<ChipPiece>>);
static_assert(std::is_same_v<LR::ListedChipEntries<SD::Entries<StandardLike>>,
                             list<>>);

// the runtime hooks over a list as Startup and SecondaryCore read it (Entries<...>)
template<typename H,
         typename... Ts>
void callEachOf(list<Ts...>*) {
    SD::callEach<H, Ts...>();
}

template<typename H,
         typename... Ts>
bool allTrueOf(list<Ts...>*) {
    return SD::allTrue<H, Ts...>();
}

template<typename... Ts>
constexpr auto* entries = static_cast<SD::Entries<Ts...>*>(nullptr);
}   // namespace

int main() {
    Kvasir::Test::test("startup_list");
    // the runtime calls of Entry.hpp: a missing bool hook is true, the first false stops the rest
    if(!SD::allTrue<RH::SecondarySync, Empty, WithPinInit>()) {
        Kvasir::Test::fail("allTrue without hooks");
    }
    if(SD::allTrue<RH::SecondarySync, Empty, SyncFalse>()) {
        Kvasir::Test::fail("allTrue with a false hook");
    }
    SD::callEach<RH::RuntimeInit, Empty, WithRuntimeInit, WithRuntimeInitLambda>();
    // a group's hooks: its members', in member order, in the group's place
    callEachOf<RH::RuntimeInit>(entries<InitN<5>, GroupNested, Group2, GroupEmpty, InitN<6>>);
    int const want[]{5, 1, 2, 1, 6};
    if(calls != 5) { Kvasir::Test::fail("group runtimeInits: count"); }
    for(int i = 0; i < 5 && i < calls; ++i) {
        if(callLog[i] != want[i]) { Kvasir::Test::fail("group runtimeInits: order"); }
    }
    if(allTrueOf<RH::SecondarySync>(entries<GroupSync>)) {
        Kvasir::Test::fail("a group's false sync");
    }
    if(!allTrueOf<RH::SecondarySync>(entries<GroupEmpty, Group2>)) {
        Kvasir::Test::fail("a group without syncs");
    }
    return Kvasir::Test::report();
}
