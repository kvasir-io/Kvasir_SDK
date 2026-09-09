// Tests for kvasir/StartUp/ListRules.hpp: the shape rules Startup and SecondaryCore apply to
// their peripheral lists. Compile time only, both ways, on fake peripherals; main() exists so
// ctest has something to run.
#include "kvasir/StartUp/ListRules.hpp"
#include "test_harness.hpp"

#include <array>

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

// ---- the offending indexes, for the reports ------------------------------------------------
template<int... Is>
using Idx = list<std::integral_constant<int, Is>...>;

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
