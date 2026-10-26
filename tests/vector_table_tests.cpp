// Tests for kvasir/StartUp/VectorTable.hpp (the vector table of both cores) and the ISR list transform
// (Entry.hpp IsrTransformOf, IsrProfiler.hpp IsrProfiling / isrProfiles). The table is compared slot for slot with
// expected tables written out per case, for three chips' interrupt traits (values copied from the chip packages'
// Interrupt.hpp as test data).
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/StartUp/IsrProfiler.hpp"
#include "kvasir/StartUp/VectorTable.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

// Nvic::UnusedIsr's function: Startup defines it on the target only.
void Kvasir::Nvic::DefaultIsrs::onIsr() { std::abort(); }

namespace {

namespace SD   = Kvasir::Startup::Detail;
namespace Nvic = Kvasir::Nvic;
using Nvic::Index;
using Nvic::Isr;

template<typename... Ts>
using list = brigand::list<Ts...>;

// ---- three chips' traits -------------------------------------------------------------------------
struct Rp2350Traits {   // begin -14, end 52; -4/-3 disabled
    static constexpr int begin = -14;
    static constexpr int end   = 52;
};

struct Rp2040Traits {
    static constexpr int begin = -14;
    static constexpr int end   = 32;
};

struct Samd21Traits {
    static constexpr int begin = -14;
    static constexpr int end   = 28;
};

// a counter, not the real clock
struct FakeTime {
    static inline std::uint32_t t       = 0;
    static inline bool          enabled = false;

    static std::uint32_t now() noexcept { return t += 10; }

    static void enable() noexcept {
        t       = 0;
        enabled = true;
    }

    // a debugger that cleared DEMCR.TRCENA since the last look: the next keepRunning() says so once
    static inline bool stopped = false;

    static bool keepRunning() noexcept {
        bool const ran = !stopped;
        stopped        = false;
        return ran;
    }
};

void stackEnd() {}

void reset() {}

template<int N>
void handler() {}

// a dispatcher-like and a wrapper-like entry: anything with `value` and `IType`
template<int I>
struct Dispatcher {
    static void onIsr() {}

    static constexpr Nvic::IsrFunctionPointer value = &onIsr;
    using IType                                     = Index<I>;
};

template<int I>
using H = Isr<&handler<I>, Index<I>>;

template<typename Traits, typename IsrList>
using New = SD::VectorTable<&stackEnd, &reset, Traits, IsrList>;

// One slot of an expected table: the vector index and the function the builder must put there.
struct Slot {
    int                      index;
    Nvic::IsrFunctionPointer isr;
};

// The expected table, written out: the initial SP, the reset entry, Nvic::UnusedIsr everywhere else but the given
// slots (slot 2 + index - begin).
template<typename Traits,
         std::size_t N>
consteval auto expected(std::array<Slot,
                                   N> const& slots) {
    std::array<Nvic::IsrFunctionPointer, New<Traits, list<>>::entries> t{};
    t.fill(Nvic::UnusedIsr::value);
    t[0] = &stackEnd;
    t[1] = &reset;
    for(auto const& s : slots) { t[2 + static_cast<std::size_t>(s.index - Traits::begin)] = s.isr; }
    return t;
}

template<typename Traits,
         typename IsrList,
         std::size_t N>
consteval bool builds(std::array<Slot,
                                 N> const& slots) {
    return New<Traits, IsrList>{}.data == expected<Traits>(slots);
}

template<typename IsrList,
         std::size_t N>
consteval bool buildsOnAllChips(std::array<Slot,
                                           N> const& slots) {
    return builds<Rp2350Traits, IsrList>(slots) && builds<Rp2040Traits, IsrList>(slots)
        && builds<Samd21Traits, IsrList>(slots);
}

// the cases: empty, the edges, core exceptions, unsorted, dispatcher- and wrapper-like entries
static_assert(buildsOnAllChips<list<>>(std::array<Slot,
                                                  0>{}));
static_assert(buildsOnAllChips<list<H<-14>>>(std::array{
  Slot{-14,
       &handler<-14>}
}));   // begin
static_assert(buildsOnAllChips<list<H<27>>>(std::array{
  Slot{27,
       &handler<27>}
}));   // the D21's end - 1
static_assert(builds<Rp2040Traits,
                     list<H<31>>>(std::array{
  Slot{31,
       &handler<31>}
}));
static_assert(builds<Rp2350Traits,
                     list<H<51>>>(std::array{
  Slot{51,
       &handler<51>}
}));
static_assert(buildsOnAllChips<list<H<-13>,
                                    H<-11>,
                                    H<-5>,
                                    H<-2>,
                                    H<-1>>>(   // core exceptions
  std::array{
    Slot{-13,
         &handler<-13>},
    Slot{-11,
         &handler<-11>},
    Slot{ -5,
         &handler<-5> },
    Slot{ -2,
         &handler<-2> },
    Slot{ -1,
         &handler<-1> }
}));
static_assert(buildsOnAllChips<list<H<20>,
                                    H<3>,
                                    H<-1>,
                                    H<0>,
                                    H<11>>>(   // unsorted
  std::array{
    Slot{20,
         &handler<20>},
    Slot{ 3,
         &handler<3> },
    Slot{-1,
         &handler<-1>},
    Slot{ 0,
         &handler<0> },
    Slot{11,
         &handler<11>}
}));
static_assert(buildsOnAllChips<list<Dispatcher<7>,
                                    H<2>,
                                    Dispatcher<-2>>>(std::array{
  Slot{ 7,
       &Dispatcher<7>::onIsr },
  Slot{ 2,
       &handler<2>           },
  Slot{-2,
       &Dispatcher<-2>::onIsr}
}));
static_assert(buildsOnAllChips<list<Kvasir::Startup::IsrProfileWrapper<&handler<5>,
                                                                       Index<5>,
                                                                       FakeTime,
                                                                       0>,
                                    H<6>>>(std::array{
  Slot{5,
       &Kvasir::Startup::IsrProfileWrapper<&handler<5>,
       Index<5>,
       FakeTime, 0>::onIsr},
  Slot{6,
       &handler<6>}
}));

// a hand-written table: the RP2040's, three ISRs
constexpr std::array<Nvic::IsrFunctionPointer, 48> expectedRp2040 = [] {
    std::array<Nvic::IsrFunctionPointer, 48> t{};
    t.fill(Nvic::UnusedIsr::value);
    t[0]      = &stackEnd;
    t[1]      = &reset;
    t[2 + 0]  = &handler<-14>;   // index -14, slot 2
    t[2 + 13] = &handler<-1>;    // SysTick, slot 15
    t[2 + 45] = &handler<31>;    // the last IRQ, slot 47
    return t;
}();
static_assert(New<Rp2040Traits,
                  list<H<31>,
                       H<-1>,
                       H<-14>>>{}
                .data
              == expectedRp2040);
static_assert(New<Rp2350Traits,
                  list<>>::entries
              == 68);
static_assert(New<Samd21Traits,
                  list<>>::entries
              == 44);

// the alignment of a SecondaryCore's table: 512 for the RP2350's 68 words of 4 bytes (on the host the words are
// pointer-sized)
constexpr std::size_t word = sizeof(Nvic::IsrFunctionPointer);
static_assert(SD::vectorTableAlignment(68) == 128 * word);
static_assert(SD::vectorTableAlignment(48) == 64 * word);
static_assert(SD::vectorTableAlignment(4) == 128);

// ---- the ISR list transform -----------------------------------------------------------------------
struct Plain {
    [[maybe_unused]] static constexpr int initStepPinConfig = 0;
};

using Policy    = Kvasir::Startup::ProfileIndicesPolicy<Index<3>{}, Index<-1>{}>;
using Profiling = Kvasir::Startup::IsrProfiling<Policy, FakeTime>;
using Isrs      = list<H<-1>, Dispatcher<2>, H<3>, H<4>>;

template<int I, unsigned Core = 0>
using W = Kvasir::Startup::IsrProfileWrapper<&handler<I>, Index<I>, FakeTime, Core>;

// no entry with a transform: the same list (the same type)
static_assert(std::is_same_v<SD::IsrTransformOf<Isrs,
                                                0,
                                                Plain>::type,
                             Isrs>);
static_assert(std::is_same_v<SD::IsrTransformOf<Isrs,
                                                0>::type,
                             Isrs>);
// IsrProfiling: only the chosen indexes become wrappers (a dispatcher passes through), in list order
static_assert(std::is_same_v<SD::IsrTransformOf<Isrs,
                                                0,
                                                Plain,
                                                Profiling>::type,
                             list<W<-1>,
                                  Dispatcher<2>,
                                  W<3>,
                                  H<4>>>);
static_assert(std::is_same_v<SD::IsrTransformInT<Isrs,
                                                 0,
                                                 list<Profiling,
                                                      Plain>>::type,
                             list<W<-1>,
                                  Dispatcher<2>,
                                  W<3>,
                                  H<4>>>);
static_assert(SD::hasIsrTransform<Profiling> && !SD::hasIsrTransform<Plain>);
// without a cycle counter (the host), the default time source is refused by name, not by an incomplete type
static_assert(KVASIR_CORE_HAS_CYCLE_COUNTER
              || !Kvasir::Startup::Detail::isCompleteTimeSource<Kvasir::Startup::DwtTimeSource>);

// the table built from it holds the wrappers in their slots
using Profiled = SD::IsrTransformOf<Isrs, 0, Profiling>::type;
static_assert(New<Rp2040Traits,
                  Profiled>{}
                .data[2 + (3 + 14)]
              == &W<3>::onIsr);
static_assert(New<Rp2040Traits,
                  Profiled>{}
                .data[2 + (-1 + 14)]
              == &W<-1>::onIsr);
static_assert(New<Rp2040Traits,
                  Profiled>{}
                .data[2 + (4 + 14)]
              == &handler<4>);

// a Startup stand-in: what isrProfiles reads (wrappers listed out of index order on purpose)
struct FakeStartup {
    using IsrList = SD::IsrTransformOf<list<H<3>, H<4>, H<-1>>, 0, Profiling>::type;
};

// a SecondaryCore stand-in: the same entry on core 1 wraps with core 1's records
struct FakeCore1 {
    using IsrList = SD::IsrTransformOf<list<H<3>>, 1, Profiling>::type;
};

static_assert(std::is_same_v<FakeCore1::IsrList,
                             list<W<3,
                                    1>>>);
static_assert(!std::is_same_v<W<3,
                                1>::Stats,
                              W<3,
                                0>::Stats>);   // one record per core

static_assert(Kvasir::Startup::isrProfiledCount<FakeStartup> == 2);
}   // namespace

int main() {
    Kvasir::Test::test("vector_table: isrProfiles");
    Profiling::preEnableRuntimeInit();
    CHECK(FakeTime::enabled);
    W<3>::onIsr();
    W<3>::onIsr();
    W<-1>::onIsr();
    auto const p = Kvasir::Startup::isrProfiles<FakeStartup>();
    CHECK_EQ(p.size(), 2U);
    CHECK_EQ(p[0].isrIndex, -1);   // table order, not list order
    CHECK_EQ(p[1].isrIndex, 3);
    CHECK_EQ(p[0].callCount, 1U);
    CHECK_EQ(p[1].callCount, 2U);
    CHECK_EQ(p[1].minDurationCycles, 10U);   // one now() on entry, one on exit
    CHECK_EQ(p[1].minIntervalCycles, 20U);

    Kvasir::Test::test("vector_table: isrProfiles per core");
    W<3, 1>::onIsr();
    auto const c1 = Kvasir::Startup::isrProfiles<FakeCore1>();
    CHECK_EQ(c1.size(), 1U);
    CHECK_EQ(c1[0].isrIndex, 3);
    CHECK_EQ(c1[0].callCount, 1U);   // core 1's record: core 0's two calls are not in it
    CHECK_EQ(Kvasir::Startup::isrProfiles<FakeStartup>()[1].callCount, 2U);

    Kvasir::Test::test("vector_table: a counter that stood still drops its samples, not the calls");
    using S7 = W<7>::Stats;
    W<7>::onIsr();   // the first call: no interval yet
    FakeTime::stopped = true;
    W<7>::
      onIsr();   // the counter stood still before this entry: its interval is dropped, its duration kept
    W<7>::onIsr();   // interval from the last entry, which the counter ran on from: 20
    auto s7 = S7::snapshot();
    CHECK_EQ(s7.callCount, 3U);
    CHECK_EQ(s7.minIntervalCycles, 20U);
    CHECK_EQ(s7.avgIntervalCycles, 20U);   // over the one counted interval, not over callCount - 1
    CHECK_EQ(s7.avgDurationCycles, 10U);
    auto const at = s7.lastCallTime + 20;
    S7::record(at,
               at,
               true,
               false);   // stood still inside the call: no duration, no base for the next interval
    S7::record(at + 100, at + 110, true, true);
    s7 = S7::snapshot();
    CHECK_EQ(s7.callCount, 5U);
    CHECK_EQ(s7.minDurationCycles, 10U);   // the 0 of the stopped call is not a minimum
    CHECK_EQ(s7.maxIntervalCycles, 20U);   // 1100 - 1000 was not measured
    CHECK_EQ(s7.avgIntervalCycles, 20U);
    return Kvasir::Test::report();
}
