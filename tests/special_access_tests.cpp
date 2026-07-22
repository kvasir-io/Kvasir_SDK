// Tests for the set/clear/reset factories and for the write-ignored-if-zero /
// write-ignored-if-one address masks, plus compile-time checks of the mask utilities.
#include "test_registers.hpp"

#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

template<typename T,
         unsigned V>
constexpr auto ctv() {
    return Kvasir::Register::value<T, static_cast<T>(V)>();
}

// compile-time checks of the mask helpers
static_assert(maskFromRange(7,
                            0)
              == 0xFFU);
static_assert(maskFromRange(9,
                            9)
              == 0x200U);
static_assert(maskFromRange(31,
                            0)
              == 0xFFFFFFFFU);
static_assert(maskFromRange(7,
                            0,
                            15,
                            12)
              == 0xF0FFU);
static_assert(Kvasir::Register::Detail::maskStartsAt(0xFF00U) == 8U);
static_assert(Kvasir::Register::Detail::onlyOneBitSet(0x200U));
static_assert(!Kvasir::Register::Detail::onlyOneBitSet(0x300U));

// set on a single read-write bit: RMW that only sets this bit
static void setSingleBit() {
    test("setSingleBit");

    recorder.setReadValue(CtrlReg::Addr::value, 0xAC);

    apply(set(CtrlReg::en));

    checkActions({
      R{CtrlReg::Addr::value, 0xAC},
      W{CtrlReg::Addr::value, 0xAD}
    });
}

// clear on a single read-write bit: RMW that only clears this bit
static void clearSingleBit() {
    test("clearSingleBit");

    recorder.setReadValue(CtrlReg::Addr::value, 0xFF);

    apply(clear(CtrlReg::irq));

    checkActions({
      R{CtrlReg::Addr::value, 0xFF},
      W{CtrlReg::Addr::value, 0xFD}
    });
}

// reset on a write-one-to-clear bit writes a one to exactly that bit
static void resetWriteOneToClearBit() {
    test("resetWriteOneToClearBit");

    recorder.setReadValue(CtrlReg::Addr::value, 0xF0);

    apply(reset(CtrlReg::flag));

    checkActions({
      R{CtrlReg::Addr::value, 0xF0},
      W{CtrlReg::Addr::value, 0xF8}
    });
}

// set, clear and a field write to the same register merge into a single RMW
static void setClearWriteMerge() {
    test("setClearWriteMerge");

    using divT = typename decltype(CtrlReg::div)::DataType;

    recorder.setReadValue(CtrlReg::Addr::value, 0xFFFF);

    apply(set(CtrlReg::en), clear(CtrlReg::irq), write(CtrlReg::div, ctv<divT, 0x7F>()));

    // en=1, irq=0, div=0x7F, flag (bit 3) and bits 15..12 preserved
    checkActions({
      R{CtrlReg::Addr::value, 0xFFFF},
      W{CtrlReg::Addr::value, 0xF7FD}
    });
}

// covering all normal RW bits of MaskedReg: no read needed even for runtime values,
// write-ignored-if-one bits are written as ones, write-ignored-if-zero bits as zeros
static void maskedRegNoRmwWhenRwBitsCovered() {
    test("maskedRegNoRmwWhenRwBitsCovered");

    recorder.setReadValue(MaskedReg::Addr::value, 0xDEADBEEF);

    apply(write(MaskedReg::low, runtimeValue(2)), write(MaskedReg::high, runtimeValue(1)));

    // low=2, high=1 (bits 3..2), bits 7..4 write one (no change), the rest write zero
    checkActions({
      W{MaskedReg::Addr::value, 0xF6}
    });
}

// a partial write reads the register and only preserves the normal RW bits
static void maskedRegPartialWrite() {
    test("maskedRegPartialWrite");

    recorder.setReadValue(MaskedReg::Addr::value, 0xDEADBEEF);

    apply(write(MaskedReg::low, runtimeValue(3)));

    // low=3, high preserved from the read (0xEF -> bits 3..2 = 0b11), bits 7..4 write
    // one, everything above (including the w1c done flag) writes zero
    checkActions({
      R{MaskedReg::Addr::value, 0xDEADBEEF},
      W{MaskedReg::Addr::value,       0xFF}
    });
}

// reset of a w1c flag inside the write-ignored-if-zero area: only that flag gets a one,
// the normal RW bits are preserved via RMW
static void maskedRegResetW1cFlag() {
    test("maskedRegResetW1cFlag");

    recorder.setReadValue(MaskedReg::Addr::value, 0xABCD);

    apply(reset(MaskedReg::done));

    // done=1 (bit 8), low/high preserved (0xD), bits 7..4 write one
    checkActions({
      R{MaskedReg::Addr::value, 0xABCD},
      W{MaskedReg::Addr::value,  0x1FD}
    });
}

// compile-time writes to all RW bits of MaskedReg: single write, fixed bits filled in
static void maskedRegCompileTimeWrite() {
    test("maskedRegCompileTimeWrite");

    using lowT  = typename decltype(MaskedReg::low)::DataType;
    using highT = typename decltype(MaskedReg::high)::DataType;

    apply(write(MaskedReg::low, ctv<lowT, 1>()), write(MaskedReg::high, ctv<highT, 2>()));

    // low=1, high=2 -> bits 3..0 = 0b1001, bits 7..4 write one
    checkActions({
      W{MaskedReg::Addr::value, 0xF9}
    });
}

// clear on a oneToToggle bit must write the bit's current value back: 1 toggles the
// bit to 0, 0 leaves it at 0 - either way the bit ends up cleared
static void clearToggleBitThatIsSet() {
    test("clearToggleBitThatIsSet");

    recorder.setReadValue(ToggleReg::Addr::value, 1U << 5);

    apply(clear(ToggleReg::pin5));

    checkActions({
      R{ToggleReg::Addr::value, 1U << 5},
      W{ToggleReg::Addr::value, 1U << 5}
    });
}

static void clearToggleBitThatIsClear() {
    test("clearToggleBitThatIsClear");

    recorder.setReadValue(ToggleReg::Addr::value, 0);

    apply(clear(ToggleReg::pin5));

    checkActions({
      R{ToggleReg::Addr::value, 0},
      W{ToggleReg::Addr::value, 0}
    });
}

// clearing two toggle bits merges into a single read+write; the untouched toggle bits
// of the register are written as zero (no change)
static void clearTwoToggleBitsMerge() {
    test("clearTwoToggleBitsMerge");

    // pin5 set, pin6 clear, unrelated toggle bit 0 set
    recorder.setReadValue(ToggleReg::Addr::value, (1U << 5) | 1U);

    apply(clear(ToggleReg::pin5, ToggleReg::pin6));

    // pin5 toggles to 0, pin6 written 0 (stays 0), bit 0 written 0 (no change)
    checkActions({
      R{ToggleReg::Addr::value, (1U << 5) | 1U},
      W{ToggleReg::Addr::value,        1U << 5}
    });
}

// in a register without write-ignored masks the plain read-write neighbors are written
// back unchanged
static void clearTogglePreservesPlainNeighbors() {
    test("clearTogglePreservesPlainNeighbors");

    recorder.setReadValue(PlainToggleReg::Addr::value, 0xE3);   // tgl=1, plain bits 0xC3

    apply(clear(PlainToggleReg::tgl));

    // everything is written back: tgl toggles to 0, the plain bits keep their value
    checkActions({
      R{PlainToggleReg::Addr::value, 0xE3},
      W{PlainToggleReg::Addr::value, 0xE3}
    });
}

int main() {
    setSingleBit();
    clearSingleBit();
    resetWriteOneToClearBit();
    clearToggleBitThatIsSet();
    clearToggleBitThatIsClear();
    clearTwoToggleBitsMerge();
    clearTogglePreservesPlainNeighbors();
    setClearWriteMerge();
    maskedRegNoRmwWhenRwBitsCovered();
    maskedRegPartialWrite();
    maskedRegResetW1cFlag();
    maskedRegCompileTimeWrite();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
