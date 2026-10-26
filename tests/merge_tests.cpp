// apply()'s merge rules (Apply.hpp, the table at mergedKind): which actions of one register in one
// step become one access, and which stay separate. A toggle merges only with a toggle, an atomic
// write only with an atomic write; a read result comes from its own read alone, writes of the same
// register never touch it. Runs on the SDK's default atomic executor (a read-modify-write), so it
// needs no chip package.
#include "test_registers.hpp"

#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// a toggle and a runtime write of one register: two read-modify-writes, the toggle first (xors run
// before writes); merged into one, the toggle bit would be OR-ed instead of flipped
void toggleWithRuntimeWrite() {
    test("toggleWithRuntimeWrite");
    recorder.setReadValues(CtrlReg::Addr::value, {0x1, 0x0});

    apply(toggle(CtrlReg::en), write(CtrlReg::div, runtimeValue(5U)));

    checkActions({
      R{CtrlReg::Addr::value,  0x1},
      W{CtrlReg::Addr::value,  0x0},
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value, 0x50}
    });
}

// the same with a literal write of a third field: the literal merges with the runtime write, the
// toggle stays apart
void toggleWithLiteralAndRuntime() {
    test("toggleWithLiteralAndRuntime");
    recorder.setReadValues(CtrlReg::Addr::value, {0x1, 0x0});

    apply(toggle(CtrlReg::en), write(CtrlReg::div, runtimeValue(5U)), set(CtrlReg::irq));

    checkActions({
      R{CtrlReg::Addr::value,  0x1},
      W{CtrlReg::Addr::value,  0x0},
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value, 0x52}
    });
}

// an atomic write and a plain runtime write of one register stay two accesses: merged, the atomic
// write would lose its executor (the chip's alias store, or the masked read-modify-write)
void atomicWithRuntimeWrite() {
    test("atomicWithRuntimeWrite");
    recorder.setReadValues(CtrlReg::Addr::value, {0x0, 0x0});

    apply(atomic(set(CtrlReg::en)), write(CtrlReg::div, runtimeValue(3U)));

    CHECK_EQ(readCount(CtrlReg::Addr::value), 2U);
    CHECK_EQ(writeCount(CtrlReg::Addr::value), 2U);
}

// the merges that stay: atomic + atomic, literal + runtime, runtime + runtime (disjoint), literal
// + literal - one access each
void atomicMergesWithAtomic() {
    test("atomicMergesWithAtomic");
    recorder.setReadValue(CtrlReg::Addr::value, 0x0);

    apply(atomic(set(CtrlReg::en), set(CtrlReg::irq)));

    checkActions({
      R{CtrlReg::Addr::value, 0x0},
      W{CtrlReg::Addr::value, 0x3}
    });
}

void literalMergesWithRuntime() {
    test("literalMergesWithRuntime");
    recorder.setReadValue(CtrlReg::Addr::value, 0x0);

    apply(set(CtrlReg::en), write(CtrlReg::div, runtimeValue(5U)));

    checkActions({
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value, 0x51}
    });
}

void runtimeMergesWithRuntime() {
    test("runtimeMergesWithRuntime");
    recorder.setReadValue(ThirdReg::Addr::value, 0x0);

    apply(write(ThirdReg::mode, runtimeValue(0x11U)),
          write(ThirdReg::control, runtimeValue(0x22U)));

    checkActions({
      R{ThirdReg::Addr::value,      0x0},
      W{ThirdReg::Addr::value, 0x110022}
    });
}

void literalMergesWithLiteral() {
    test("literalMergesWithLiteral");
    recorder.setReadValue(CtrlReg::Addr::value, 0x0);

    apply(set(CtrlReg::en), set(CtrlReg::irq));

    checkActions({
      R{CtrlReg::Addr::value, 0x0},
      W{CtrlReg::Addr::value, 0x3}
    });
}

// a literal, a runtime write and a second runtime write of other bits: one access
void literalBetweenTwoRuntimeWrites() {
    test("literalBetweenTwoRuntimeWrites");
    recorder.setReadValue(ThirdReg::Addr::value, 0x0);

    apply(write(ThirdReg::mode, runtimeValue(0x11U)),
          write(ThirdReg::config, value<std::uint32_t, 0x22>()),
          write(ThirdReg::control, runtimeValue(0x33U)));

    checkActions({
      W{ThirdReg::Addr::value, 0x112233}
    });
}

// two toggles of different bits merge into one xor
void togglesOfDifferentBitsMerge() {
    test("togglesOfDifferentBitsMerge");
    recorder.setReadValue(CtrlReg::Addr::value, 0x1);

    apply(toggle(CtrlReg::en), toggle(CtrlReg::irq));

    checkActions({
      R{CtrlReg::Addr::value, 0x1},
      W{CtrlReg::Addr::value, 0x2}
    });
}

// a read and a write of one register in one step: the read sees the value before the write, and
// the result is that value alone, not OR-ed with what was written
void readNotOredWithWrite() {
    test("readNotOredWithWrite");
    recorder.setReadValues(CtrlReg::Addr::value, {0x0, 0x0});

    auto const r = apply(read(CtrlReg::en), set(CtrlReg::en));

    CHECK_EQ(r.template get<0>(), 0U);
}

void readNotOredWithRuntimeWrite() {
    test("readNotOredWithRuntimeWrite");
    recorder.setReadValue(SecondReg::Addr::value, 0x01);

    auto const r = apply(read(SecondReg::data), write(SecondReg::data, runtimeValue(0x80U)));

    CHECK_EQ(r.template get<0>(), 0x01U);
}

// one register read in two steps: two results, one each
void twoReadsTwoSlots() {
    test("twoReadsTwoSlots");
    recorder.setReadValues(SecondReg::Addr::value, {0x01, 0x02});

    auto const r = apply(read(SecondReg::data), sequencePoint, read(SecondReg::data));

    CHECK_EQ(r.template get<0>(), 0x01U);
    CHECK_EQ(r.template get<1>(), 0x02U);
}

// two fields of one register read in one step share the one read
void twoFieldsOneRead() {
    test("twoFieldsOneRead");
    recorder.setReadValue(SecondReg::Addr::value, 0xABCD);

    auto const r = apply(read(SecondReg::data), read(SecondReg::status));

    CHECK_EQ(readCount(SecondReg::Addr::value), 1U);
    CHECK_EQ(r[SecondReg::data], 0xCDU);
    CHECK_EQ(r[SecondReg::status], 0xABU);
}
}   // namespace

// Register::fromReset<R>: literal writes of R's fields from two calls (two Startup entries of one step) merge into
// ONE store, no read, the bits nobody names at R's reset value (0xF002: irq and bits 15:12) - whatever the register
// holds
void fromResetOneStore() {
    test("fromResetOneStore");
    recorder.setReadValue(ResetReg::Addr::value, 0xFFFF'FFFFU);

    apply(fromReset<ResetReg>(set(ResetReg::en)),
          fromReset<ResetReg>(write(ResetReg::div, value<3U>())));

    checkActions({
      W{ResetReg::Addr::value, 0x0000'F033U}
    });
}

// a named field wins over its reset value: irq cleared, the rest of the fill stays
void fromResetNamedFieldWins() {
    test("fromResetNamedFieldWins");

    apply(fromReset<ResetReg>(clear(ResetReg::irq)));

    checkActions({
      W{ResetReg::Addr::value, 0x0000'F000U}
    });
}

int main() {
    toggleWithRuntimeWrite();
    toggleWithLiteralAndRuntime();
    atomicWithRuntimeWrite();
    atomicMergesWithAtomic();
    literalMergesWithRuntime();
    runtimeMergesWithRuntime();
    literalMergesWithLiteral();
    literalBetweenTwoRuntimeWrites();
    togglesOfDifferentBitsMerge();
    readNotOredWithWrite();
    readNotOredWithRuntimeWrite();
    twoReadsTwoSlots();
    twoFieldsOneRead();
    fromResetOneStore();
    fromResetNamedFieldWins();

    return Kvasir::Test::report();
}
