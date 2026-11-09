// Register::SharedState (Register/Exec.hpp) and apply()'s refusal of one step that writes the same bits of one shared
// state through two registers (Apply.hpp, findStateConflict). The registers below are a GPIO group shaped like the SAM
// PORT's (chip_atsam_common RegisterSeam.hpp): OUT and its one-to-clear, one-to-set and one-to-toggle registers, all of
// them the state OUT. A step runs by address, highest first: OUTSET (0x308) before OUTCLR (0x304), so `reset(clr::pin3),
// set(set::pin3)` - "make it an output, low; now high" - would end low. The plan marks such a list
// (MergeError::oneStateTwoRegisters, apply()'s static_assert, the message rows in diagnostics_must_fail); the lists that
// stay legal are checked here at compile time and run on the mock.
#include "test_registers.hpp"

using namespace Kvasir::Register;
using namespace Kvasir::Test;
namespace D = Kvasir::Register::Detail;

namespace {
using ClearAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToClear>;
using SetAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToSet>;
using ToggleAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToToggle>;
// RP SIO's form (chip_rp_common Io.hpp): GPIO_OUT_CLR / _SET hold one write-only field of every pin
using AllClearAccess
  = Access<AccessType::writeOnly, ReadActionType::normal, ModifiedWriteValueType::oneToClear>;
using AllSetAccess
  = Access<AccessType::writeOnly, ReadActionType::normal, ModifiedWriteValueType::oneToSet>;

struct OutReg {
    using Addr = Address<0x300, 0x00000000, 0x00000000, std::uint32_t>;
    static constexpr Field<Addr, 31, 0> out{};
};

struct OutClrReg {
    using Addr = Address<0x304, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr Field<Addr, 3, 3, ClearAccess>     pin3{};
    static constexpr Field<Addr, 4, 4, ClearAccess>     pin4{};
    static constexpr Field<Addr, 31, 0, AllClearAccess> all{};
};

struct OutSetReg {
    using Addr = Address<0x308, 0xFFFFFFFF, 0x00000000, std::uint32_t>;
    static constexpr Field<Addr, 3, 3, SetAccess>     pin3{};
    static constexpr Field<Addr, 4, 4, SetAccess>     pin4{};
    static constexpr Field<Addr, 31, 0, AllSetAccess> all{};
};

struct OutTglReg {
    using Addr
      = Address<0x30C, 0xFFFFFFFF, 0x00000000, std::uint32_t, Mode<{.writeBackActs = 0xFFFFFFFFU}>>;
    static constexpr Field<Addr, 3, 3, ToggleAccess> pin3{};
};

static_assert(distinctAddresses<OutReg,
                                OutClrReg,
                                OutSetReg,
                                OutTglReg,
                                CtrlReg>());
}   // namespace

// the hook for this TU, as a chip's register seam header gives it (before the first apply() of these registers)
template<unsigned A, unsigned Z, unsigned O, typename T, typename M>
    requires(A >= 0x300 && A <= 0x30C)
struct Kvasir::Register::SharedState<Kvasir::Tag::User, Kvasir::Register::Address<A, Z, O, T, M>> {
    static constexpr unsigned value = 0x300;
};

namespace {
template<typename... Args>
constexpr D::MergeError errorOf = D::PlanOf<Args...>::plan.error;

template<typename... Args>
constexpr D::StateConflict conflictOf = D::PlanOf<Args...>::plan.conflict;

using E = D::MergeError;

using Clr3    = decltype(reset(OutClrReg::pin3));
using Clr4    = decltype(reset(OutClrReg::pin4));
using Set3    = decltype(set(OutSetReg::pin3));
using Set4    = decltype(set(OutSetReg::pin4));
using Tgl3    = decltype(toggle(OutTglReg::pin3));
using OutRt   = decltype(write(OutReg::out, 1U));
using OutRead = decltype(read(OutReg::out));
using EnSet   = decltype(set(CtrlReg::en));

// the hook reaches the plan: every register of the group is the state OUT, any other its own
static_assert(D::PlanOf<Set3>::rec[0].state == 0x300);
static_assert(D::PlanOf<OutRead>::rec[0].state == 0x300);
static_assert(D::PlanOf<EnSet>::rec[0].state == 0);

// refused: one pin's level through two registers in one step, in either argument order, in a list too
static_assert(errorOf<Clr3,
                      Set3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<Set3,
                      Clr3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<brigand::list<Clr3,
                                    Clr4>,
                      Set3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<Tgl3,
                      Set3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<Clr3,
                      Tgl3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<OutRt,
                      Set3>
              == E::oneStateTwoRegisters);   // the state's own register is one of them
// the pair in execution order (OUTSET 0x308 runs before OUTCLR 0x304) and the shared bit
static_assert(conflictOf<Clr3, Set3>.first == 1 && conflictOf<Clr3, Set3>.last == 0
              && conflictOf<Clr3, Set3>.mask == 0x8);
// three of them: the pair of the highest address
static_assert(conflictOf<Clr3, Set3, Tgl3>.first == 2 && conflictOf<Clr3, Set3, Tgl3>.last == 1);

// legal: different bits, the same register, two steps, a read, a register of its own state
static_assert(errorOf<Clr3,
                      Set4>
              == E::none);
static_assert(errorOf<Clr3,
                      Clr4>
              == E::none);
static_assert(errorOf<Set3,
                      Set3>
              == E::none);
static_assert(errorOf<Clr3,
                      SequencePoint,
                      Set3>
              == E::none);
static_assert(errorOf<OutRead,
                      Set3>
              == E::none);
static_assert(errorOf<EnSet,
                      Set3,
                      Clr4>
              == E::none);
// a list with another merge error reports that one (the first found)
static_assert(errorOf<decltype(set(CtrlReg::en)),
                      decltype(clear(CtrlReg::en)),
                      Clr3,
                      Set3>
              == E::literalsDisagree);

// RP's form: one pin's bit written into the field of every pin. A zero in a write-ignored-if-zero bit writes nothing
// (Apply.hpp writtenBits), so two pins' writes stay legal and only the ones of a pin meet; a plain register's zeros do
// write (OUT = 0x8 clears pin 4), and a runtime value may hold any bit
using AllClr3  = decltype(write(OutClrReg::all, value<0x8U>()));
using AllSet4  = decltype(write(OutSetReg::all, value<0x10U>()));
using AllSet34 = decltype(write(OutSetReg::all, value<0x18U>()));
using AllSetRt = decltype(write(OutSetReg::all, 1U));
using OutLit3  = decltype(write(OutReg::out, value<0x8U>()));

static_assert(errorOf<AllClr3,
                      AllSet4>
              == E::none);
static_assert(errorOf<AllClr3,
                      Set4>
              == E::none);
static_assert(errorOf<AllClr3,
                      AllSet34>
              == E::oneStateTwoRegisters);
static_assert(conflictOf<AllClr3, AllSet34>.mask == 0x8);   // the shared one, not the fields' masks
static_assert(errorOf<AllClr3,
                      Set3>
              == E::oneStateTwoRegisters);
static_assert(errorOf<AllSetRt,
                      Clr4>
              == E::oneStateTwoRegisters);
static_assert(errorOf<OutLit3,
                      Set4>
              == E::oneStateTwoRegisters);
static_assert(conflictOf<OutLit3, Set4>.mask == 0x10);

// the legal lists run as before: address descending, the two steps in order
void differentPinsOneStep() {
    test("differentPinsOneStep");
    apply(reset(OutClrReg::pin3), set(OutSetReg::pin4));
    checkActions({
      W{OutSetReg::Addr::value, 0x10},
      W{OutClrReg::Addr::value,  0x8}
    });
}

void onePinTwoSteps() {
    test("onePinTwoSteps");
    apply(reset(OutClrReg::pin3), sequencePoint, set(OutSetReg::pin3));
    checkActions({
      W{OutClrReg::Addr::value, 0x8},
      W{OutSetReg::Addr::value, 0x8}
    });
}

void twoPinsWholeFields() {
    test("twoPinsWholeFields");
    apply(write(OutClrReg::all, value<0x8U>()), write(OutSetReg::all, value<0x10U>()));
    checkActions({
      W{OutSetReg::Addr::value, 0x10},
      W{OutClrReg::Addr::value,  0x8}
    });
}

void onePinTwoApplies() {
    test("onePinTwoApplies");
    apply(reset(OutClrReg::pin3));
    apply(set(OutSetReg::pin3));
    checkActions({
      W{OutClrReg::Addr::value, 0x8},
      W{OutSetReg::Addr::value, 0x8}
    });
}
}   // namespace

int main() {
    differentPinsOneStep();
    onePinTwoSteps();
    onePinTwoApplies();
    twoPinsWholeFields();
    return Kvasir::Test::report();
}
