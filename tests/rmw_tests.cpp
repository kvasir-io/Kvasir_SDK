// Tests for read-modify-write behavior: when a read is needed, which bits are preserved
// and how injected register contents merge with written fields.
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

// an explicit read plus writes to the same register: the explicit read stays separate
// from the RMW read (cmd, bit 8, is not covered by the writes, so a RMW read is needed)
template<typename Reg>
static void explicitReadPlusRmwWrite() {
    test("explicitReadPlusRmwWrite");

    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x21);

    apply(read(Reg::dat), write(Reg::dat, v), write(Reg::stop, ctv<stopT, 1>()));

    checkActions({
      R{Reg::Addr::value,             0},
      R{Reg::Addr::value,             0},
      W{Reg::Addr::value, v | (1U << 9)}
    });
}

// RMW merges the injected register content into the written value: bit 8 comes from the
// read, dat and stop from the write actions
template<typename Reg>
static void rmwMergesInjectedValue() {
    test("rmwMergesInjectedValue");

    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x44);

    recorder.setReadValue(Reg::Addr::value, 0x100);   // bit 8 set

    apply(write(Reg::dat, v), write(Reg::stop, ctv<stopT, 1>()));

    checkActions({
      R{Reg::Addr::value,                     0x100},
      W{Reg::Addr::value, v | (1U << 8) | (1U << 9)}
    });
}

// writing one field preserves the other readable bits of the register
static void rmwPreservesUnwrittenBits() {
    test("rmwPreservesUnwrittenBits");

    recorder.setReadValue(ThirdReg::Addr::value, 0xAABBCC);

    apply(write(ThirdReg::mode, runtimeValue(0x42)));

    // bits 31..16 are write-ignored-if-zero, so only control and config are preserved
    checkActions({
      R{ThirdReg::Addr::value, 0xAABBCC},
      W{ThirdReg::Addr::value, 0x42BBCC}
    });
}

// two runtime writes covering all bits below the write-ignored area need no read
static void noRmwWhenAllFieldsCovered() {
    test("noRmwWhenAllFieldsCovered");

    recorder.setReadValue(SecondReg::Addr::value, 0xCDAB);

    apply(write(SecondReg::data, runtimeValue(0x12)), write(SecondReg::status, runtimeValue(0x34)));

    checkActions({
      W{SecondReg::Addr::value, 0x3412}
    });
}

// writing non-adjacent fields preserves the field in between
static void rmwNonAdjacentFields() {
    test("rmwNonAdjacentFields");

    recorder.setReadValue(ThirdReg::Addr::value, 0xCCBBAA);

    apply(write(ThirdReg::control, runtimeValue(0x11)), write(ThirdReg::mode, runtimeValue(0x33)));

    checkActions({
      R{ThirdReg::Addr::value, 0xCCBBAA},
      W{ThirdReg::Addr::value, 0x33BB11}
    });
}

// a compile-time and a runtime write to the same field or together
static void rmwRuntimeMergesWithCompileTime() {
    test("rmwRuntimeMergesWithCompileTime");

    recorder.setReadValue(ThirdReg::Addr::value, 0xFFFFFF);

    apply(write(ThirdReg::mode, Kvasir::Register::value<std::uint32_t, 0x99>()),
          write(ThirdReg::mode, runtimeValue(0x42)));

    // mode = 0x99 | 0x42 = 0xDB, control/config preserved from the read
    checkActions({
      R{ThirdReg::Addr::value, 0xFFFFFF},
      W{ThirdReg::Addr::value, 0xDBFFFF}
    });
}

// RMW on two registers in one apply: each register gets its own read+write pair
static void rmwMultipleRegisters() {
    test("rmwMultipleRegisters");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0xFFFF);
    recorder.setReadValue(ThirdReg::Addr::value, 0x555555);

    apply(write(SimpleTestReg::dat, runtimeValue(0x11)), write(ThirdReg::mode, runtimeValue(0x33)));

    CHECK(recorder.actions.size() == 4);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(ThirdReg::Addr::value) == 1);
    // dat = 0x11, cmd/stop (bits 9..8) preserved from the read
    CHECK_EQ(writtenValue(SimpleTestReg::Addr::value), 0x311U);
    // mode = 0x33, control/config preserved from the read
    CHECK_EQ(writtenValue(ThirdReg::Addr::value), 0x335555U);
}

// runtime writes to a field outside the writable mask still perform RMW and preserve
// the rest of the register
static void rmwPartialRegisterWrite() {
    test("rmwPartialRegisterWrite");

    recorder.setReadValue(ThirdReg::Addr::value, 0x123456);

    apply(write(ThirdReg::config, runtimeValue(0xAB)));

    // config = 0xAB, control preserved, bits 31..16 write-ignored-if-zero
    checkActions({
      R{ThirdReg::Addr::value, 0x123456},
      W{ThirdReg::Addr::value,   0xAB56}
    });
}

// compile-time writes to the same register merge into one write no matter where a
// read of that register appears in the argument list; the reads always execute first
static void writeMergeIndependentOfReadPosition() {
    test("writeMergeIndependentOfReadPosition");

    using cmdT  = typename decltype(SimpleTestReg::cmd)::DataType;
    using stopT = typename decltype(SimpleTestReg::stop)::DataType;

    // explicit read + RMW read + one merged write, identical for every argument order
    std::vector<Kvasir::Test::Recorder::Action> const expected{
      R{SimpleTestReg::Addr::value,  0xAB},
      R{SimpleTestReg::Addr::value,  0xAB},
      W{SimpleTestReg::Addr::value, 0x3AB}
    };

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
          read(SimpleTestReg::dat),
          write(SimpleTestReg::stop, ctv<stopT, 1>()));
    checkActions(expected);

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
          write(SimpleTestReg::stop, ctv<stopT, 1>()),
          read(SimpleTestReg::dat));
    checkActions(expected);

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(read(SimpleTestReg::dat),
          write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
          write(SimpleTestReg::stop, ctv<stopT, 1>()));
    checkActions(expected);
}

int main() {
    explicitReadPlusRmwWrite<SimpleTestReg>();
    explicitReadPlusRmwWrite<ComplexTestReg>();

    rmwMergesInjectedValue<SimpleTestReg>();
    rmwMergesInjectedValue<ComplexTestReg>();

    rmwPreservesUnwrittenBits();
    noRmwWhenAllFieldsCovered();
    rmwNonAdjacentFields();
    rmwRuntimeMergesWithCompileTime();
    rmwMultipleRegisters();
    rmwPartialRegisterWrite();
    writeMergeIndependentOfReadPosition();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
