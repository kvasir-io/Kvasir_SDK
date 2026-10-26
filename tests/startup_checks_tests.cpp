// Tests for kvasir/StartUp/Checks.hpp: the shared check block of Startup and SecondaryCore, on
// fake lists with a fake chip (vector table traits), a fake SecondaryCore and a fake log backend.
// Compile time only: a clean list's checks pass here; every rule's offender list is checked on a
// broken fixture (the reports themselves are diagnostics_must_fail's). main() exists for ctest.
#include "kvasir/StartUp/Checks.hpp"
#include "test_harness.hpp"

#include <array>
#include <type_traits>

namespace {

namespace C  = Kvasir::Startup::Checks;
namespace CD = Kvasir::Startup::Checks::Detail;
namespace LR = Kvasir::Startup::ListRules;
namespace SD = Kvasir::Startup::Detail;
using Kvasir::Startup::Diagnostics::Pair;

template<typename... Ts>
using list = brigand::list<Ts...>;

// ---- the fake chip ------------------------------------------------------------------------
struct Traits {
    static constexpr int        begin   = -14;
    static constexpr int        end     = 32;
    static constexpr std::array perCore = {16};
};

template<typename... Ps>
struct FakeSecondary {};

template<typename T>
struct IsFakeSecondary : std::false_type {};

template<typename... Ps>
struct IsFakeSecondary<FakeSecondary<Ps...>> : std::true_type {};

template<typename T>
struct FakeSecondaryPeripherals {
    using type = list<>;
};

template<typename... Ps>
struct FakeSecondaryPeripherals<FakeSecondary<Ps...>> {
    using type = list<Ps...>;
};

struct NoBackend;   // incomplete: no unlisted backend

using Ctx = C::Context<Traits, IsFakeSecondary, FakeSecondaryPeripherals, NoBackend>;

// ---- fake entries ---------------------------------------------------------------------------
template<int I>
struct FakeIsr {
    using IType = std::integral_constant<int, I>;
};

template<int I>
struct WithIsr {
    using Isr = list<FakeIsr<I>>;
};

struct Plain {
    static constexpr int initStepPinConfig = 0;
};

struct Plain2 {
    static constexpr int initStepPeripheryConfig = 0;
};

struct WithInit {
    static void runtimeInit() {}
};

struct Config {   // not a peripheral
    [[maybe_unused]] static constexpr int instance = 0;
};

struct Timeout {
    static constexpr bool isLaunchTimeout = true;
};

struct Clocks {
    static void coreClockInit() {}

    static void peripheryClockInit() {}
};

struct BoolInit {
    static bool runtimeInit() { return true; }
};

struct BothContracts {
    static constexpr std::array<int, 1> isrPriorityLevels{0};
    static constexpr bool               singleIsrPriorityLevel = true;
};

// an entry that rewrites the list's ISRs (Entry.hpp IsrTransform), as IsrProfiling does
struct Transform {
    template<typename Isrs, unsigned Core>
    using IsrTransform = Isrs;

    static void preEnableRuntimeInit() {}
};

struct Transform2 {
    template<typename Isrs, unsigned Core>
    using IsrTransform = list<>;

    static void preEnableRuntimeInit() {}
};

using Core1 = FakeSecondary<WithIsr<5>, Plain2, Timeout>;

// ---- clean lists pass --------------------------------------------------------------------------
static_assert(C::CheckList<Ctx,
                           C::Role::primary,
                           0,
                           void,
                           list<Plain,
                                WithIsr<3>,
                                Core1>,
                           list<Plain,
                                WithIsr<3>,
                                Core1>>::value);
static_assert(C::CheckList<Ctx,
                           C::Role::secondary,
                           1,
                           void,
                           list<WithIsr<5>,
                                Plain2,
                                Timeout>,
                           list<WithIsr<5>,
                                Plain2,
                                Timeout>>::value);
static_assert(C::ImageChecks<Ctx,
                             void,
                             Clocks,
                             list<Plain,
                                  WithIsr<3>,
                                  Core1>,
                             list<Plain,
                                  WithIsr<3>,
                                  Core1>>::value);
static_assert(C::CheckList<Ctx,
                           C::Role::primary,
                           0,
                           void,
                           list<>,
                           list<>>::value);

// one IsrTransform in a list passes, core 1's too
static_assert(C::CheckList<Ctx,
                           C::Role::secondary,
                           1,
                           void,
                           list<Plain2,
                                Transform>,
                           list<Plain2,
                                Transform>>::value);
// one IsrTransform in core 0's list passes
static_assert(C::CheckList<Ctx,
                           C::Role::primary,
                           0,
                           void,
                           list<Plain,
                                Transform,
                                WithIsr<3>>,
                           list<Plain,
                                Transform,
                                WithIsr<3>>>::value);

// ---- the shape rules' offenders, per role ----------------------------------------------------
template<int Core>
using OnCore = std::integral_constant<int, Core>;

static_assert(std::is_same_v<CD::OnCore<0,
                                        LR::DuplicateEntries<list<Plain,
                                                                  Plain2,
                                                                  Plain,
                                                                  Plain>>>,
                             list<Pair<Plain,
                                       OnCore<0>>>>);
static_assert(std::is_same_v<CD::OnCore<1,
                                        LR::NotPeripherals<IsFakeSecondary,
                                                           list<Plain,
                                                                Config>>>,
                             list<Pair<Config,
                                       OnCore<1>>>>);
static_assert(std::is_same_v<LR::ExtraSecondaries<IsFakeSecondary,
                                                  list<Core1,
                                                       Plain,
                                                       FakeSecondary<>>>,
                             list<FakeSecondary<>>>);
static_assert(std::is_same_v<LR::Secondaries<IsFakeSecondary,
                                             list<Plain,
                                                  FakeSecondary<>>>,
                             list<FakeSecondary<>>>);
static_assert(std::is_same_v<LR::LaunchTimeouts<list<Plain,
                                                     Timeout>>,
                             list<Timeout>>);
// IsrTransform: at most one per list (TwoIsrTransforms names the second), on either core
static_assert(std::is_same_v<LR::IsrTransformEntries<list<Plain,
                                                          WithIsr<3>>>,
                             list<>>);
static_assert(std::is_same_v<LR::IsrTransformEntries<list<Plain,
                                                          Transform>>,
                             list<Transform>>);
static_assert(std::is_same_v<LR::ExtraIsrTransforms<list<Transform>>,
                             list<>>);
static_assert(std::is_same_v<LR::ExtraIsrTransforms<list<Transform,
                                                         Plain,
                                                         Transform2>>,
                             list<Transform2>>);
static_assert(std::is_same_v<CD::OnCore<1,
                                        LR::ExtraIsrTransforms<list<Transform2,
                                                                    Plain2,
                                                                    Transform>>>,
                             list<Pair<Transform,
                                       OnCore<1>>>>);
static_assert(std::is_same_v<LR::ClockSettingsEntries<list<Plain,
                                                           Clocks>>,
                             list<Clocks>>);
static_assert(std::is_same_v<LR::WrongHookShapes<list<Plain,
                                                      BoolInit>>,
                             list<BoolInit>>);

// ---- the vector rules, per core ------------------------------------------------------------------
using TwiceIsrs = SD::IsrsOf<list<WithIsr<3>, WithIsr<3>, WithIsr<4>>>::type;
static_assert(std::is_same_v<LR::DuplicateIsrIndexes<TwiceIsrs>::type,
                             list<OnCore<3>,
                                  OnCore<3>>>);
static_assert(std::is_same_v<LR::InvalidIsrIndexes<Traits,
                                                   SD::IsrsOf<list<WithIsr<40>,
                                                                   WithIsr<3>>>::type>::type,
                             list<OnCore<40>>>);

// ---- the image rules ---------------------------------------------------------------------------------
// a peripheral on both cores names it and the SecondaryCore
static_assert(std::is_same_v<CD::OnBothCoresOffenders<Ctx,
                                                      list<Plain,
                                                           FakeSecondary<Plain,
                                                                         Plain2>>>::type,
                             list<Pair<Plain,
                                       FakeSecondary<Plain,
                                                     Plain2>>>>);
static_assert(std::is_same_v<CD::OnBothCoresOffenders<Ctx,
                                                      list<Plain,
                                                           Core1>>::type,
                             list<>>);

// a per-core entry (perCoreEntry: IsrProfiling) may be on both cores; the transform alone does not make it one
struct PerCoreTransform : Transform {
    static constexpr bool perCoreEntry = true;
};

static_assert(std::is_same_v<CD::OnBothCoresOffenders<Ctx,
                                                      list<PerCoreTransform,
                                                           FakeSecondary<PerCoreTransform>>>::type,
                             list<>>);
static_assert(std::is_same_v<CD::OnBothCoresOffenders<Ctx,
                                                      list<Transform,
                                                           FakeSecondary<Transform>>>::type,
                             list<Pair<Transform,
                                       FakeSecondary<Transform>>>>);
// a runtimeInit after the SecondaryCore
static_assert(std::is_same_v<CD::LateRuntimeInits<Ctx,
                                                  list<WithInit,
                                                       Core1,
                                                       Plain,
                                                       WithInit>>::type,
                             list<Pair<WithInit,
                                       void>>>);
static_assert(std::is_same_v<CD::LateRuntimeInits<Ctx,
                                                  list<WithInit,
                                                       Core1>>::type,
                             list<>>);
// a chip-wide vector on both cores; a per-core one (16 here, or a core exception) is fine
static_assert(std::is_same_v<CD::VectorsOnBothCores<Ctx,
                                                    list<WithIsr<5>,
                                                         WithIsr<16>>,
                                                    list<WithIsr<5>,
                                                         FakeSecondary<WithIsr<5>,
                                                                       WithIsr<16>>>>::type,
                             list<Pair<FakeSecondary<WithIsr<5>,
                                                     WithIsr<16>>,
                                       OnCore<5>>>>);
static_assert(std::is_same_v<CD::VectorsOnBothCores<Ctx,
                                                    list<WithIsr<3>>,
                                                    list<WithIsr<3>,
                                                         Core1>>::type,
                             list<>>);

// ... also when the SecondaryCore's entry serves it as a sub-interrupt (SharedIsr.hpp: its dispatcher is generated
// in the SecondaryCore, the list here is as written); a per-core one stays fine
template<int I>
struct WithSubIsr {
    using SubIsrs = list<FakeIsr<I>>;
};

static_assert(std::is_same_v<CD::VectorsOnBothCores<Ctx,
                                                    list<WithIsr<5>,
                                                         WithIsr<16>>,
                                                    list<WithIsr<5>,
                                                         FakeSecondary<WithSubIsr<5>,
                                                                       WithSubIsr<16>>>>::type,
                             list<Pair<FakeSecondary<WithSubIsr<5>,
                                                     WithSubIsr<16>>,
                                       OnCore<5>>>>);
// both contracts on one entry
static_assert(std::is_same_v<C::EachWith<void,
                                         Kvasir::MPL::Filter<CD::DeclaresBothContracts,
                                                             list<Plain,
                                                                  BothContracts>>>::type,
                             list<Pair<BothContracts,
                                       void>>>);
// the per-core lists: core 0's, then the SecondaryCore's
static_assert(std::is_same_v<C::CoreLists<Ctx,
                                          list<Clocks,
                                               Plain,
                                               Core1>,
                                          Plain,
                                          Core1>,
                             list<list<Clocks,
                                       Plain,
                                       Core1>,
                                  list<WithIsr<5>,
                                       Plain2,
                                       Timeout>>>);
// an incomplete backend is not added to the contract entries, a complete one is (once)
static_assert(std::is_same_v<SD::IsrContractEntries<list<list<Plain>>,
                                                    NoBackend>::type,
                             list<Plain>>);
static_assert(std::is_same_v<SD::IsrContractEntries<list<list<Plain>>,
                                                    BothContracts>::type,
                             list<Plain,
                                  BothContracts>>);
static_assert(std::is_same_v<SD::IsrContractEntries<list<list<BothContracts>>,
                                                    BothContracts>::type,
                             list<BothContracts>>);
}   // namespace

int main() {
    // everything above is checked at compile time; reaching main means it all held
    Kvasir::Test::test("startup_checks");
    return Kvasir::Test::report();
}
