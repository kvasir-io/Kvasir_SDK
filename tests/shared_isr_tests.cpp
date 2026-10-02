// Tests for kvasir/StartUp/SharedIsr.hpp: the dispatcher Startup generates for a vector several
// sub-interrupts share, and the grouping of a list's SubIsrs by vector.
#include "kvasir/StartUp/SharedIsr.hpp"
#include "kvasir_test.hpp"

#include <string>
#include <type_traits>

// The core's NVIC actions, which a host build has no core for: writes of the vector's bit to fake
// registers (enable 0xE0, clear pending 0xE4, priority 0xE8 = index << 8 | priority).
struct FakeNvic {
    using Enable  = Kvasir::Register::Address<0xE0, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    using Pending = Kvasir::Register::Address<0xE4, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    using Prio    = Kvasir::Register::Address<0xE8, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr Kvasir::Register::FieldLocation<Enable,
                                                     Kvasir::Register::maskFromRange(31, 0),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     std::uint32_t>
      enable{};
    static constexpr Kvasir::Register::FieldLocation<Pending,
                                                     Kvasir::Register::maskFromRange(31, 0),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     std::uint32_t>
      pending{};
    static constexpr Kvasir::Register::FieldLocation<Prio,
                                                     Kvasir::Register::maskFromRange(31, 0),
                                                     Kvasir::Register::WriteOnlyAccess,
                                                     std::uint32_t>
      prio{};
};

namespace Kvasir::Nvic {
template<int I>
struct MakeAction<Action::Enable, Index<I>>
  : decltype(MPL::list(
      Register::write(FakeNvic::enable, Register::value<std::uint32_t, (1U << I)>()))){};

template<int I>
struct MakeAction<Action::ClearPending, Index<I>>
  : decltype(MPL::list(
      Register::write(FakeNvic::pending, Register::value<std::uint32_t, (1U << I)>()))){};

template<int P, int I>
struct MakeAction<Action::SetPriority<P>, Index<I>>
  : decltype(MPL::list(
      Register::write(FakeNvic::prio, Register::value<std::uint32_t, ((I << 8) | P)>()))){};
}   // namespace Kvasir::Nvic

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

namespace {

// RP-shaped: a masked status register (INTS, read-only), a write-one-to-clear raw register
// (INTR: every bit write-ignored-if-zero) and an enable register (INTE).
struct Ints {
    using Addr = Address<0xC0, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(0, 0), ReadOnlyAccess, bool> a{};
    static constexpr FieldLocation<Addr, maskFromRange(1, 1), ReadOnlyAccess, bool> b{};
};

struct Intr {
    using Addr = Address<0xC4, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(0, 0), ROneToClearAccess, bool> a{};
    static constexpr FieldLocation<Addr, maskFromRange(1, 1), ROneToClearAccess, bool> b{};
};

struct Inte {
    using Addr = Address<0xC8, 0x00000000, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(0, 0), ReadWriteAccess, bool> a{};
    static constexpr FieldLocation<Addr, maskFromRange(1, 1), ReadWriteAccess, bool> b{};
};

// SAM-shaped: a raw flag register and its enable-set register, read together.
struct Flag {
    using Addr = Address<0xD0, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(2, 2), ROneToClearAccess, bool> c{};
};

struct EnSet {
    using Addr = Address<0xD4, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr FieldLocation<Addr, maskFromRange(2, 2), ReadWriteAccess, bool> c{};
};

using V7 = Kvasir::Nvic::Index<7>;
using V9 = Kvasir::Nvic::Index<9>;

std::string trace;

void onA() { trace += "A"; }

void onB() { trace += "B"; }

void onC() { trace += "C"; }

namespace N = Kvasir::Nvic;

using SubA = N::SubIsr<V7, &onA, N::Status<Ints::a>, N::ClearFirst<Intr::a>, N::Enable<Inte::a>>;
using SubB = N::SubIsr<V7, &onB, N::Status<Ints::b>, N::ClearLast<Intr::b>, N::Enable<Inte::b>>;
using SubC = N::SubIsr<V9, &onC, N::RawStatus<Flag::c, EnSet::c>, N::ClearFirst<Flag::c>>;

struct DriverAB {
    using SubIsrs = brigand::list<SubA, SubB>;
};

struct DriverC {
    using SubIsrs = brigand::list<SubC>;
};

struct Plain {
    [[maybe_unused]] static constexpr int initStepPinConfig = 0;
};

namespace D = Kvasir::Startup::Detail;

// grouping: one dispatcher per vector, children in list order; no SubIsrs, no dispatcher
static_assert(std::is_same_v<D::SharedDispatchers<brigand::list<Plain>>::type,
                             brigand::list<>>);
static_assert(std::is_same_v<D::SharedDispatchers<brigand::list<DriverAB,
                                                                Plain,
                                                                DriverC>>::type,
                             brigand::list<D::SharedDispatch<V7,
                                                             SubA,
                                                             SubB>,
                                           D::SharedDispatch<V9,
                                                             SubC>>>);
static_assert(std::is_same_v<D::WithSharedDispatchers<Plain>,
                             brigand::list<Plain>>);
static_assert(std::is_same_v<D::WithSharedDispatchers<DriverAB>,
                             brigand::list<DriverAB,
                                           D::SharedDispatch<V7,
                                                             SubA,
                                                             SubB>>>);
// the priority rule: none, agreed, disagreeing
using P3a = N::SubIsr<V7, &onA, N::Status<Ints::a>, N::NoClear, N::NoEnable, 3>;
using P3b = N::SubIsr<V7, &onB, N::Status<Ints::b>, N::NoClear, N::NoEnable, 3>;
using P2b = N::SubIsr<V7, &onB, N::Status<Ints::b>, N::NoClear, N::NoEnable, 2>;
static_assert(D::SharedIsrDetail::agreedPriority<SubA,
                                                 SubB>()
              == -1);
static_assert(D::SharedIsrDetail::agreedPriority<P3a,
                                                 SubB,
                                                 P3b>()
              == 3);
static_assert(D::SharedIsrDetail::agreedPriority<P3a,
                                                 P2b>()
              == -2);

using AB = D::SharedDispatch<V7, SubA, SubB>;
using C  = D::SharedDispatch<V9, SubC>;

// the second child pending only: one read of INTS, its handler, then its clear (ClearLast)
void onlySecondPending() {
    test("onlySecondPending");
    trace.clear();
    recorder.readValues[0xC0].push_back(0x2);
    AB::onIsr();
    CHECK(trace == "B");
    checkActions({
      R{0xC0, 0x2},
      W{0xC4, 0x2}
    });
}

// both pending: list order; A's clear before A, B's clear after B
void bothPending() {
    test("bothPending");
    trace.clear();
    recorder.readValues[0xC0].push_back(0x3);
    AB::onIsr();
    CHECK(trace == "AB");
    checkActions({
      R{0xC0, 0x3},
      W{0xC4, 0x1},
      W{0xC4, 0x2}
    });
}

// nothing pending: the read and nothing else
void nonePending() {
    test("nonePending");
    trace.clear();
    recorder.readValues[0xC0].push_back(0x0);
    AB::onIsr();
    CHECK(trace.empty());
    checkActions({
      R{0xC0, 0x0}
    });
}

// RawStatus: the flag alone is not enough, the enable must be set too; both registers in one apply
void rawStatusNeedsEnable() {
    test("rawStatusNeedsEnable");
    trace.clear();
    recorder.readValues[0xD0].push_back(0x4);   // FLAG.c set
    recorder.readValues[0xD4].push_back(0x0);   // ENSET.c clear
    C::onIsr();
    CHECK(trace.empty());
    CHECK(recorder.actions.size() == 2U);

    test("rawStatusDispatches");
    trace.clear();
    recorder.readValues[0xD0].push_back(0x4);
    recorder.readValues[0xD4].push_back(0x4);
    C::onIsr();
    CHECK(trace == "C");
    CHECK(recorder.actions.size() == 3U);
    CHECK(std::holds_alternative<W>(recorder.actions.back()));
    CHECK(std::get<W>(recorder.actions.back()) == (W{0xD0, 0x4}));
}

// the init step: both enable bits of INTE in one write
void enablesMerged() {
    test("enablesMerged");
    recorder.readValues[0xC8].push_back(0x0);
    apply(brigand::at_c<std::remove_cvref_t<decltype(AB::initStepInterruptConfig)>, 0>{});
    bool sawWrite = false;
    for(auto const& a : recorder.actions) {
        if(auto const* w = std::get_if<W>(&a); w != nullptr && w->address == 0xC8) {
            CHECK(w->value == 0x3U);
            CHECK(!sawWrite);
            sawWrite = true;
        }
    }
    CHECK(sawWrite);
}

}   // namespace

int main() {
    onlySecondPending();
    bothPending();
    nonePending();
    rawStatusNeedsEnable();
    enablesMerged();
    return report();
}
