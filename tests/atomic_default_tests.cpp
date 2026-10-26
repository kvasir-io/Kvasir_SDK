// Register::atomic() on the SDK's default executor (Exec.hpp, RegisterExec<... AtomicWriteLiteralAction>): the
// read-modify-write a chip without an atomic alias block gets (SAM, STM32). One read and one write when other bits
// must be kept, a single write when every other bit of the register ignores a written zero, and a flag the read
// returned is not written back. merge_tests.cpp has the merges (atomic + atomic, atomic + runtime write).
//
// The interrupt mask around the read-modify-write (IrqMasked) is not visible here: under the register mock it is an
// empty struct. On a target it is `mrs primask; cpsid i` ... `msr primask; isb` around the ldr/str.
#include "test_registers.hpp"

#include <type_traits>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// the other fields of the register are kept: one read, then one write with only `en` changed
void setKeepsTheOthers() {
    test("setKeepsTheOthers");
    recorder.setReadValue(CtrlReg::Addr::value, 0xA52);   // div 0xA5, irq set, en clear

    apply(atomic(set(CtrlReg::en)));

    checkActions({
      R{CtrlReg::Addr::value, 0xA52},
      W{CtrlReg::Addr::value, 0xA53}
    });
}

void clearKeepsTheOthers() {
    test("clearKeepsTheOthers");
    recorder.setReadValue(CtrlReg::Addr::value, 0xA53);

    apply(atomic(clear(CtrlReg::irq)));

    checkActions({
      R{CtrlReg::Addr::value, 0xA53},
      W{CtrlReg::Addr::value, 0xA51}
    });
}

void multiBitLiteral() {
    test("multiBitLiteral");
    recorder.setReadValue(CtrlReg::Addr::value, 0xFF3);

    apply(atomic(write(CtrlReg::div, value<std::uint32_t, 0x12>())));

    checkActions({
      R{CtrlReg::Addr::value, 0xFF3},
      W{CtrlReg::Addr::value, 0x123}
    });
}
}   // namespace

// A register with a write-one-to-clear flag, described as svd_converter emits it: the flag's bit in the
// write-ignored-if-zero mask and in Semantics::writeBackActs (CtrlReg's flag has neither, so it is not used here).
namespace fixture {   // named: an unused field in an unnamed namespace is a warning
using W1cAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToClear>;
using TriggerAccess
  = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToSet>;

struct FlagCtrlReg {
    using Addr = Address<0x100,
                         0x00000008U,
                         0x00000000U,
                         std::uint32_t,
                         Mode<{.writeBackActs = 0x00000008U}>>;
    static constexpr FieldLocation<Addr, maskFromRange(0, 0), ReadWriteAccess, std::uint32_t> en{};
    static constexpr FieldLocation<Addr, maskFromRange(3, 3), W1cAccess, std::uint32_t> flag{};
};

// PIO CTRL's shape: per-machine enables [3:0] and two one-to-set (self-clearing) restart triggers [7:4] and [11:8], as
// svd_converter emits them (the triggers in the zero mask and in writeBackActs)
struct TriggerCtrlReg {
    using Addr = Address<0x104,
                         0xFFFFFFF0U,
                         0x00000000U,
                         std::uint32_t,
                         Mode<{.writeBackActs = 0x00000FF0U}>>;
    static constexpr FieldLocation<Addr, maskFromRange(3, 0), ReadWriteAccess, std::uint32_t>
      enable{};
    static constexpr FieldLocation<Addr, maskFromRange(7, 4), TriggerAccess, std::uint32_t>
      restart{};
    static constexpr FieldLocation<Addr, maskFromRange(11, 8), TriggerAccess, std::uint32_t>
      clkRestart{};
};
}   // namespace fixture

using fixture::FlagCtrlReg;
using fixture::TriggerCtrlReg;
static_assert(distinctFromShared<FlagCtrlReg>);
static_assert(distinctFromShared<TriggerCtrlReg>);

namespace {
// a pending flag the read found set is written as 0: the atomic set must not clear it on the way
void flagNotWrittenBack() {
    test("flagNotWrittenBack");
    recorder.setReadValue(FlagCtrlReg::Addr::value, 0x0F8);

    apply(atomic(set(FlagCtrlReg::en)));

    checkActions({
      R{FlagCtrlReg::Addr::value, 0x0F8},
      W{FlagCtrlReg::Addr::value, 0x0F1}
    });
}

// atomic set of one bit of a one-to-set trigger: the read-modify-write keeps the enables as read and writes the
// other triggers 0 (writeBackActs) - a restart of machine 1 only, whatever the read returned in 11:4
void triggerSetWritesOnlyItsBit() {
    test("triggerSetWritesOnlyItsBit");
    recorder.setReadValue(TriggerCtrlReg::Addr::value,
                          0x0F5A);   // enables 0xA, junk in the trigger bits

    apply(atomic(set(bit<1>(TriggerCtrlReg::restart))));

    checkActions({
      R{TriggerCtrlReg::Addr::value, 0x0F5A},
      W{TriggerCtrlReg::Addr::value, 0x002A}
    });
}

// SimpleTestReg's bits 31..10 ignore a written zero: writing all of stop, cmd and dat needs no read
void noReadWhenNothingIsKept() {
    test("noReadWhenNothingIsKept");

    apply(atomic(set(SimpleTestReg::stop),
                 clear(SimpleTestReg::cmd),
                 write(SimpleTestReg::dat, value<std::uint32_t, 0x5A>())));

    checkActions({
      W{SimpleTestReg::Addr::value, 0x25A}
    });
}

// ... but writing only some of them keeps the rest: one read
void readWhenSomethingIsKept() {
    test("readWhenSomethingIsKept");
    recorder.setReadValue(SimpleTestReg::Addr::value, 0x0C3);

    apply(atomic(set(SimpleTestReg::stop)));

    checkActions({
      R{SimpleTestReg::Addr::value, 0x0C3},
      W{SimpleTestReg::Addr::value, 0x2C3}
    });
}
}   // namespace

// a step of atomic writes only returns nothing to read
static_assert(std::is_void_v<decltype(apply(atomic(set(CtrlReg::en))))>);

int main() {
    setKeepsTheOthers();
    clearKeepsTheOthers();
    multiBitLiteral();
    flagNotWrittenBack();
    triggerSetWritesOnlyItsBit();
    noReadWhenNothingIsKept();
    readWhenSomethingIsKept();
    return report();
}
