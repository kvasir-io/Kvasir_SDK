// Tests for the Kvasir::Register action factories and the utility layer they are built on:
// read/set/clear/reset/write including their variadic forms, the mask helpers and the
// FieldLocation type traits.
//
// The existing write/read/rmw/special_access suites drive apply() with single actions; this
// one focuses on the factories themselves, in particular the variadic overloads that produce
// lists of actions.
#include "test_registers.hpp"

#include <cstdint>
#include <type_traits>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using W = Recorder::Write;
using R = Recorder::Read;

namespace mpl = Kvasir::MPL;

// ===========================================================================
// mask helpers
// ===========================================================================

static_assert(maskFromRange(0,
                            0)
              == 0x00000001U);
static_assert(maskFromRange(1,
                            0)
              == 0x00000003U);
static_assert(maskFromRange(7,
                            0)
              == 0x000000FFU);
static_assert(maskFromRange(9,
                            9)
              == 0x00000200U);
static_assert(maskFromRange(15,
                            8)
              == 0x0000FF00U);
static_assert(maskFromRange(31,
                            0)
              == 0xFFFFFFFFU);
static_assert(maskFromRange(31,
                            31)
              == 0x80000000U);
static_assert(maskFromRange(11,
                            4)
              == 0x00000FF0U);

// the variadic form or-s several ranges together
static_assert(maskFromRange(1,
                            0,
                            5,
                            4)
              == 0x00000033U);
static_assert(maskFromRange(0,
                            0,
                            2,
                            2,
                            4,
                            4)
              == 0x00000015U);

static_assert(Detail::maskStartsAt(0x00000001U) == 0);
static_assert(Detail::maskStartsAt(0x00000002U) == 1);
static_assert(Detail::maskStartsAt(0x000000FFU) == 0);
static_assert(Detail::maskStartsAt(0x0000FF00U) == 8);
static_assert(Detail::maskStartsAt(0x00000FF0U) == 4);
static_assert(Detail::maskStartsAt(0x80000000U) == 31);

static_assert(Detail::onlyOneBitSet(0x1U));
static_assert(Detail::onlyOneBitSet(0x80000000U));
static_assert(!Detail::onlyOneBitSet(0x0U));
static_assert(!Detail::onlyOneBitSet(0x3U));
static_assert(!Detail::onlyOneBitSet(0xFFU));

static_assert(Detail::orAllOf() == 0U);
static_assert(Detail::orAllOf(0x1U) == 0x1U);
static_assert(Detail::orAllOf(0x1U,
                              0x2U)
              == 0x3U);
static_assert(Detail::orAllOf(0x1U,
                              0x2U,
                              0x4U,
                              0x8U)
              == 0xFU);

// ===========================================================================
// FieldLocation traits
// ===========================================================================

using EnField     = std::decay_t<decltype(CtrlReg::en)>;
using DivField    = std::decay_t<decltype(CtrlReg::div)>;
using FlagField   = std::decay_t<decltype(CtrlReg::flag)>;
using StopField   = std::decay_t<decltype(SimpleTestReg::stop)>;
using DatField    = std::decay_t<decltype(SimpleTestReg::dat)>;
using ToggleField = std::decay_t<decltype(ToggleReg::pin5)>;

static_assert(Detail::IsFieldLocation<EnField>::value);
static_assert(Detail::IsFieldLocation<DatField>::value);
static_assert(!Detail::IsFieldLocation<int>::value);
static_assert(!Detail::IsFieldLocation<CtrlReg>::value);

static_assert(Detail::IsWritable<EnField>::value);

// ---- access: what the factories refuse ---------------------------------------------------
// The literal write/set/clear factories and read() assert these, so the traits are what a
// negative case can check. Read-only is not writable; write-only and write-once are not
// readable; the once variants are otherwise ordinary.
using RoField     = std::decay_t<decltype(AccessTestReg::ro)>;
using WoField     = std::decay_t<decltype(AccessTestReg::wo)>;
using RwField     = std::decay_t<decltype(AccessTestReg::rw)>;
using WOnceField  = std::decay_t<decltype(AccessTestReg::wonce)>;
using RwOnceField = std::decay_t<decltype(AccessTestReg::rwonce)>;

static_assert(!Detail::IsWritable<RoField>::value);
static_assert(Detail::IsWritable<WoField>::value);   // write only is still writable
static_assert(Detail::IsWritable<RwField>::value);
static_assert(Detail::IsWritable<WOnceField>::value);
static_assert(Detail::IsWritable<RwOnceField>::value);
static_assert(!Detail::IsWritable<int>::value);

static_assert(Detail::IsReadable<RoField>::value);
static_assert(!Detail::IsReadable<WoField>::value);
static_assert(Detail::IsReadable<RwField>::value);
static_assert(!Detail::IsReadable<WOnceField>::value);
static_assert(Detail::IsReadable<RwOnceField>::value);
static_assert(!Detail::IsReadable<int>::value);

// the toggle special case of clear() is on the access type directly
static_assert(Detail::accessWritable(AccessType::readWrite));
static_assert(!Detail::accessWritable(AccessType::readOnly));

// ---- a literal must fit its field ------------------------------------------------------
// write(field, value<V>()) shifts V into place; a V wider than the field would lose its top
// bits to the shift before the post-shift mask check ever sees them.
static_assert(Detail::literalFits(0x00000001U,
                                  0));
static_assert(Detail::literalFits(0x00000001U,
                                  1));
static_assert(!Detail::literalFits(0x00000001U,
                                   2));
static_assert(Detail::literalFits(0x00000FF0U,
                                  0xFF));
static_assert(!Detail::literalFits(0x00000FF0U,
                                   0x100));
static_assert(Detail::literalFits(0x80000000U,
                                  1));
static_assert(!Detail::literalFits(0x80000000U,
                                   3));   // bit 31 alone: 3 << 31 keeps only one bit
static_assert(Detail::literalFits(0xFFFFFFFFU,
                                  0xFFFFFFFFU));

// ---- two literals in one step must agree where they overlap -----------------------------
// apply merges literal writes to one register by OR; the merge refuses two that disagree
// on a shared bit (set + clear of one bit, two values for one field).
static_assert(Detail::literalsAgree<0x1U,
                                    0x1U,
                                    0x1U,
                                    0x1U>);   // set twice
static_assert(!Detail::literalsAgree<0x1U,
                                     0x1U,
                                     0x1U,
                                     0x0U>);   // set + clear
static_assert(Detail::literalsAgree<0x1U,
                                    0x1U,
                                    0x2U,
                                    0x0U>);   // set + clear, other bits
static_assert(Detail::literalsAgree<0xFF0U,
                                    0x030U,
                                    0xFF0U,
                                    0x030U>);
static_assert(!Detail::literalsAgree<0xFF0U,
                                     0x030U,
                                     0xFF0U,
                                     0x010U>);   // 3 and 1 in one field
static_assert(Detail::literalsAgree<0xFF0U,
                                    0x030U,
                                    0x00FU,
                                    0x005U>);   // disjoint fields
static_assert(!Detail::literalsAgree<0xFFFU,
                                     0x030U,
                                     0x0F0U,
                                     0x000U>);   // clear inside a write
// ... except on bits the register ignores a zero on: two pins' writes to a set/clear alias
// (one whole-register field each) merge into one write by design
static_assert(Detail::literalsAgree<0xFFFFFFFFU,
                                    0x40U,
                                    0xFFFFFFFFU,
                                    0x80U,
                                    0xFFFFFFFFU>);
static_assert(!Detail::literalsAgree<0xFFFFFFFFU,
                                     0x40U,
                                     0xFFFFFFFFU,
                                     0x80U,
                                     0x0000000FU>);
static_assert(Detail::literalsAgree<0x1U,
                                    0x1U,
                                    0x1U,
                                    0x0U,
                                    0x1U>);
static_assert(!Detail::literalsAgree<0x1U,
                                     0x1U,
                                     0x1U,
                                     0x0U,
                                     0x2U>);

// ---- the register description itself ------------------------------------------------------
// a mask must be non-empty and inside the register; an address aligned to the register
static_assert(Detail::maskFitsRegister(0x1U,
                                       4));
static_assert(Detail::maskFitsRegister(0x80000000U,
                                       4));
static_assert(Detail::maskFitsRegister(0x8000U,
                                       2));
static_assert(!Detail::maskFitsRegister(0x10000U,
                                        2));
static_assert(!Detail::maskFitsRegister(0x100U,
                                        1));
static_assert(!Detail::maskFitsRegister(0x0U,
                                        4));
static_assert(Detail::addressAligned(0x40000004U,
                                     4));
static_assert(!Detail::addressAligned(0x40000002U,
                                      4));
static_assert(Detail::addressAligned(0x40000002U,
                                     2));
static_assert(Detail::addressAligned(0x40000003U,
                                     1));

// write-one-to-clear bits are the ones reset() accepts
static_assert(Detail::IsSetToClear<FlagField>::value);
static_assert(!Detail::IsSetToClear<EnField>::value);
static_assert(!Detail::IsSetToClear<ToggleField>::value);

// the mask and field type are recoverable from a location
static_assert(Detail::GetMask<EnField>::value == 0x1U);
static_assert(Detail::GetMask<DivField>::value == 0xFF0U);
static_assert(Detail::GetMask<StopField>::value == 0x200U);
static_assert(std::is_same_v<Detail::GetFieldTypeT<DatField>,
                             std::uint32_t>);
static_assert(std::is_same_v<Detail::GetFieldTypeT<std::decay_t<decltype(ComplexTestReg::stop)>>,
                             ComplexTestReg::STOPVal>);

// ... and from the action a factory produced
static_assert(Detail::GetMask<decltype(set(CtrlReg::en))>::value == 0x1U);
static_assert(Detail::GetAddress<decltype(set(CtrlReg::en))>::value == 0x50U);
static_assert(Detail::GetAddress<EnField>::value == 0x50U);
static_assert(Detail::GetAddress<StopField>::value == 0x10U);

// the write-ignored masks come from the Address
static_assert(Detail::GetAddress<DatField>::writeIgnoredIfZeroMask == 0xFFFFFC00U);
static_assert(Detail::GetAddress<EnField>::writeIgnoredIfZeroMask == 0x00000000U);
static_assert(Detail::GetAddress<std::decay_t<decltype(MaskedReg::low)>>::writeIgnoredIfOneMask
              == 0x000000F0U);

// action classification predicates
static_assert(Detail::IsReadPred<decltype(read(CtrlReg::en))>::value);
static_assert(!Detail::IsReadPred<decltype(set(CtrlReg::en))>::value);
static_assert(Detail::IsRuntimeWritePred<decltype(write(CtrlReg::div,
                                                        std::uint32_t{1}))>::value);
static_assert(!Detail::IsRuntimeWritePred<decltype(set(CtrlReg::en))>::value);

// ===========================================================================
// the variadic factory overloads
// ===========================================================================

// several single bit sets in one call, all in the same register: the result is a list of
// actions which apply merges into a single write
static void variadicSet() {
    test("variadicSet");

    apply(set(CtrlReg::en, CtrlReg::irq));

    // CtrlReg has no write-ignored bits, so setting two of them is a read-modify-write
    checkActionKinds("rw");
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0x3U);
}

static void variadicClear() {
    test("variadicClear");

    recorder.setReadValue(CtrlReg::Addr::value, 0xFFFFFFFF);
    apply(clear(CtrlReg::en, CtrlReg::irq));

    checkActionKinds("rw");
    // both bits cleared, everything else preserved
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0xFFFFFFFCU);
}

static void variadicRead() {
    test("variadicRead");

    recorder.setReadValue(CtrlReg::Addr::value, 0x00000FF3);
    auto const results = apply(read(CtrlReg::en, CtrlReg::irq, CtrlReg::div));

    // all three fields live in the same register, so one read serves all of them
    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);      // en   = bit 0
    CHECK_EQ(get<1>(results), 1U);      // irq  = bit 1
    CHECK_EQ(get<2>(results), 0xFFU);   // div  = bits 11..4
}

static void variadicReset() {
    test("variadicReset");

    // reset writes a one to a write-one-to-clear bit
    apply(reset(CtrlReg::flag));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(CtrlReg::Addr::value) & 0x8U, 0x8U);
}

// the variadic write overload takes compile time FieldValues
static void variadicFieldValueWrite() {
    test("variadicFieldValueWrite");

    apply(write(ComplexTestReg::CMDValC::read, ComplexTestReg::STOPValC::enable));

    // cmd and stop are bits 9..8; bits 7..0 (dat) are normal read-write bits that this
    // apply does not cover, so a read-modify-write is required
    checkActions({
      R{ComplexTestReg::Addr::value,                     0},
      W{ComplexTestReg::Addr::value, (1U << 8) | (1U << 9)}
    });
}

// a list produced by a variadic factory can be nested inside another apply
static void nestedActionLists() {
    test("nestedActionLists");

    apply(set(CtrlReg::en, CtrlReg::irq), write(CtrlReg::div, value<std::uint32_t, 0xA>()));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0x3U | (0xAU << 4));
}

// ===========================================================================
// runtime writes
// ===========================================================================

// a runtime value must be masked down to the field, bits above the field are dropped
static void runtimeWriteMasksToField() {
    test("runtimeWriteMasksToField");

    // dat is bits 7..0, so the 0xF00 part of the value must not escape the field.
    // bits 9..8 are not covered by this apply, hence the read-modify-write.
    apply(write(SimpleTestReg::dat, runtimeValue(0xFFF)));

    checkActions({
      R{SimpleTestReg::Addr::value,     0},
      W{SimpleTestReg::Addr::value, 0xFFU}
    });
}

static void runtimeWriteShiftsIntoPlace() {
    test("runtimeWriteShiftsIntoPlace");

    // div lives at bits 11..4, so the value has to be shifted up by four
    apply(write(CtrlReg::div, runtimeValue(0xA5)));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0xA50U);
}

static void runtimeWriteZero() {
    test("runtimeWriteZero");

    recorder.setReadValue(CtrlReg::Addr::value, 0xFFFFFFFF);
    apply(write(CtrlReg::div, runtimeValue(0)));

    checkActionKinds("rw");
    // the field is cleared, every other bit survives
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0xFFFFF00FU);
}

// ===========================================================================
// compile time writes
// ===========================================================================

static void compileTimeWriteShiftsIntoPlace() {
    test("compileTimeWriteShiftsIntoPlace");

    apply(write(CtrlReg::div, value<std::uint32_t, 0xA5>()));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(CtrlReg::Addr::value), 0xA50U);
}

static void fieldValueWriteSingle() {
    test("fieldValueWriteSingle");

    apply(write(ComplexTestReg::STOPValC::enable));

    // only bit 9 is written, bits 8..0 are normal read-write bits and must be preserved
    checkActions({
      R{ComplexTestReg::Addr::value,       0},
      W{ComplexTestReg::Addr::value, 1U << 9}
    });
}

// enum typed fields accept a runtime value of the enum type
static void runtimeWriteEnumField() {
    test("runtimeWriteEnumField");

    auto const v = static_cast<ComplexTestReg::CMDVal>(runtimeValue(1));
    apply(write(ComplexTestReg::cmd, v));

    checkActions({
      R{ComplexTestReg::Addr::value,       0},
      W{ComplexTestReg::Addr::value, 1U << 8}
    });
}

// ===========================================================================
// set / clear / reset on individual bits
// ===========================================================================

static void setInWriteIgnoredRegister() {
    test("setInWriteIgnoredRegister");

    // ToggleReg is write-ignored-if-zero across the whole register, so setting a single
    // bit needs no read
    apply(set(ToggleReg::pin5));

    checkActions({
      W{ToggleReg::Addr::value, 1U << 5}
    });
}

static void setTwoBitsInWriteIgnoredRegister() {
    test("setTwoBitsInWriteIgnoredRegister");

    apply(set(ToggleReg::pin5, ToggleReg::pin6));

    checkActions({
      W{ToggleReg::Addr::value, (1U << 5) | (1U << 6)}
    });
}

// ===========================================================================
// apply itself
// ===========================================================================

static void applyWithNoActions() {
    test("applyWithNoActions");

    apply();

    checkActions({});
}

static void applyReturnsReadResults() {
    test("applyReturnsReadResults");

    recorder.setReadValue(CtrlReg::Addr::value, 0x00000FF1);
    auto const results = apply(read(CtrlReg::en), read(CtrlReg::div));

    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);
    CHECK_EQ(get<1>(results), 0xFFU);
}

// reads and writes to the same register in one apply keep their relative order
static void readThenWriteSameRegister() {
    test("readThenWriteSameRegister");

    // the explicit read and the read-modify-write each read the register, so both need a
    // value injected
    recorder.setReadValues(CtrlReg::Addr::value, {0x00000002, 0x00000002});
    auto const results = apply(read(CtrlReg::irq), set(CtrlReg::en));

    CHECK_EQ(get<0>(results), 1U);
    CHECK_EQ(writeCount(CtrlReg::Addr::value), 1U);
    // en is set and the previously set irq bit survives the read-modify-write
    CHECK_EQ(writtenValue(CtrlReg::Addr::value) & 0x3U, 0x3U);
}

// ===========================================================================
// Note on AtomicFactories.hpp and IsolatedFactories.hpp -- deliberately untested
//
// Register::atomic() and Register::isolated() cannot currently be called at all, so there
// is no behaviour to pin down:
//
//   * every overload is constrained on Detail::IsWriteLiteral<T> or Detail::IsWriteRuntime<T>,
//     and both of those traits are declared only as a primary template deriving from
//     std::false_type. There is no specialisation anywhere in the tree, so the constraint
//     is false for every T and all overloads are removed from the overload set.
//   * two of the four atomic() overloads additionally declare template parameters (U, Ts...)
//     that appear nowhere in the parameter list, so they could never be deduced even if the
//     constraint held.
//   * all of them return void and have empty bodies. AtomicFactories.hpp says as much:
//     "warning these are still a work in progress!".
//
// Tests should be added here once the traits are specialised and the functions do something.
// ===========================================================================

int main() {
    variadicSet();
    variadicClear();
    variadicRead();
    variadicReset();
    variadicFieldValueWrite();
    nestedActionLists();

    runtimeWriteMasksToField();
    runtimeWriteShiftsIntoPlace();
    runtimeWriteZero();

    compileTimeWriteShiftsIntoPlace();
    fieldValueWriteSingle();
    runtimeWriteEnumField();

    setInWriteIgnoredRegister();
    setTwoBitsInWriteIgnoredRegister();

    applyWithNoActions();
    applyReturnsReadResults();
    readThenWriteSameRegister();

    return Kvasir::Test::report();
}
