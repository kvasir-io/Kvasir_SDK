// Tests combining reads and writes across several registers in a single apply, with
// runtime and compile-time values in various orders.
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

// SecondReg needs no RMW (its upper bits are write-ignored), SimpleTestReg does; the
// plain write is scheduled first
static void writesToTwoRegisters() {
    test("writesToTwoRegisters");

    auto const v1 = runtimeValue(0x12);
    auto const v2 = runtimeValue(0x34);

    apply(write(SimpleTestReg::dat, v1), write(SecondReg::data, v2));

    checkActions({
      W{    SecondReg::Addr::value, v2},
      R{SimpleTestReg::Addr::value,  0},
      W{SimpleTestReg::Addr::value, v1}
    });
}

static void mixedReadsAndWritesTwoRegisters() {
    test("mixedReadsAndWritesTwoRegisters");

    using stopT = typename decltype(SimpleTestReg::stop)::DataType;

    auto const v1 = runtimeValue(0x21);
    auto const v2 = runtimeValue(0x43);

    apply(read(SimpleTestReg::dat),
          write(SimpleTestReg::dat, v1),
          write(SimpleTestReg::stop, ctv<stopT, 1>()),
          write(SecondReg::data, v2),
          write(SecondReg::status, runtimeValue(0xFF)));

    // SecondReg merges both fields into one plain write; SimpleTestReg has the explicit
    // read, the RMW read and the merged write
    checkActions({
      W{    SecondReg::Addr::value, v2 | (0xFFU << 8)},
      R{SimpleTestReg::Addr::value,                 0},
      R{SimpleTestReg::Addr::value,                 0},
      W{SimpleTestReg::Addr::value,    v1 | (1U << 9)}
    });
}

// reads and writes over three registers in one apply
static void threeRegisterMixedReadWrite() {
    test("threeRegisterMixedReadWrite");

    using cmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);
    recorder.setReadValue(SecondReg::Addr::value, 0xABCD);
    recorder.setReadValue(ThirdReg::Addr::value, 0x112233);

    auto const result = apply(read(SimpleTestReg::dat),
                              read(SecondReg::data),
                              write(ThirdReg::control, runtimeValue(0x77)),
                              read(ThirdReg::config),
                              write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
                              read(SecondReg::status));

    CHECK(recorder.actions.size() == 7);
    CHECK(writeCount(ThirdReg::Addr::value) == 1);
    CHECK(writeCount(SimpleTestReg::Addr::value) == 1);
    CHECK(writeCount(SecondReg::Addr::value) == 0);

    CHECK_EQ(result[SimpleTestReg::dat], 0xFFU);
    CHECK_EQ(result[SecondReg::data], 0xCDU);
    CHECK_EQ(result[SecondReg::status], 0xABU);
    CHECK_EQ(result[ThirdReg::config], 0x22U);
}

// order variant 1: read, compile-time write, runtime write, read, compile-time write
static void mixedWriteTypesOrder1() {
    test("mixedWriteTypesOrder1");

    using cmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    auto const v = runtimeValue(0x66);

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1AB);
    recorder.setReadValue(SecondReg::Addr::value, 0xCD12);
    recorder.setReadValue(ThirdReg::Addr::value, 0x345678);

    auto const result
      = apply(read(SimpleTestReg::dat),
              write(SecondReg::data, Kvasir::Register::value<std::uint32_t, 0x42>()),
              write(ThirdReg::control, v),
              read(SecondReg::status),
              write(SimpleTestReg::cmd, ctv<cmdT, 1>()));

    CHECK(recorder.actions.size() == 7);
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[SecondReg::status], 0xCDU);

    CHECK_EQ(writtenValue(SecondReg::Addr::value) & 0xFFU, 0x42U);
    CHECK_EQ(writtenValue(ThirdReg::Addr::value) & 0xFFU, v);
    CHECK_EQ(writtenValue(SimpleTestReg::Addr::value) & 0x100U, 0x100U);
}

// order variant 2: runtime write, read, read, compile-time write, runtime write
static void mixedWriteTypesOrder2() {
    test("mixedWriteTypesOrder2");

    auto const v = runtimeValue(0x55);

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x3FF);
    recorder.setReadValue(SecondReg::Addr::value, 0xABCD);
    recorder.setReadValue(ThirdReg::Addr::value, 0x112233);

    auto const result
      = apply(write(SimpleTestReg::dat, v),
              read(SecondReg::data),
              read(ThirdReg::mode),
              write(SecondReg::status, Kvasir::Register::value<std::uint32_t, 0xEE>()),
              write(ThirdReg::config, v + 1));

    CHECK_EQ(result[SecondReg::data], 0xCDU);
    CHECK_EQ(result[ThirdReg::mode], 0x11U);

    CHECK_EQ(writtenValue(SimpleTestReg::Addr::value) & 0xFFU, v);
    CHECK_EQ((writtenValue(SecondReg::Addr::value) >> 8) & 0xFFU, 0xEEU);
    CHECK_EQ((writtenValue(ThirdReg::Addr::value) >> 8) & 0xFFU, v + 1);
}

// order variant 3: compile-time, read, runtime, read, compile-time, runtime, read
static void mixedWriteTypesOrder3() {
    test("mixedWriteTypesOrder3");

    using cmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    auto const v1 = runtimeValue(0x2A);
    auto const v2 = runtimeValue(0x51);

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1AB);
    recorder.setReadValue(SecondReg::Addr::value, 0x1234);
    recorder.setReadValue(ThirdReg::Addr::value, 0xABCDEF);

    auto const result = apply(write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
                              read(ThirdReg::control),
                              write(SecondReg::data, v1),
                              read(SimpleTestReg::dat),
                              write(ThirdReg::mode, Kvasir::Register::value<std::uint32_t, 0x99>()),
                              write(SecondReg::status, v2),
                              read(ThirdReg::config));

    CHECK_EQ(result[ThirdReg::control], 0xEFU);
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[ThirdReg::config], 0xCDU);

    // both runtime writes to SecondReg merge into one write
    auto const secondWritten = writtenValue(SecondReg::Addr::value);
    CHECK_EQ(secondWritten & 0xFFU, v1);
    CHECK_EQ((secondWritten >> 8) & 0xFFU, v2);
    CHECK_EQ((writtenValue(ThirdReg::Addr::value) >> 16) & 0xFFU, 0x99U);
    CHECK(writeCount(SimpleTestReg::Addr::value) == 1);
}

static void allRuntimeWritesWithReads() {
    test("allRuntimeWritesWithReads");

    auto const v1 = runtimeValue(0x13);
    auto const v2 = runtimeValue(0x57);
    auto const v3 = runtimeValue(0x9B);

    recorder.setReadValue(SimpleTestReg::Addr::value, 0xFF);
    recorder.setReadValue(SecondReg::Addr::value, 0xEEDD);
    recorder.setReadValue(ThirdReg::Addr::value, 0xCCBBAA);

    auto const result = apply(read(SimpleTestReg::dat),
                              write(ThirdReg::control, v1),
                              read(SecondReg::data),
                              write(SimpleTestReg::dat, v2),
                              read(ThirdReg::config),
                              write(SecondReg::status, v3));

    CHECK_EQ(result[SimpleTestReg::dat], 0xFFU);
    CHECK_EQ(result[SecondReg::data], 0xDDU);
    CHECK_EQ(result[ThirdReg::config], 0xBBU);

    CHECK_EQ(writtenValue(SimpleTestReg::Addr::value) & 0xFFU, v2);
    CHECK_EQ((writtenValue(SecondReg::Addr::value) >> 8) & 0xFFU, v3);
    CHECK_EQ(writtenValue(ThirdReg::Addr::value) & 0xFFU, v1);
}

static void allCompileTimeWritesWithReads() {
    test("allCompileTimeWritesWithReads");

    using cmdT  = typename decltype(SimpleTestReg::cmd)::DataType;
    using stopT = typename decltype(SimpleTestReg::stop)::DataType;

    // enough values for the explicit reads and the RMW reads
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    recorder.setReadValue(SecondReg::Addr::value, 0x5678);
    recorder.setReadValues(ThirdReg::Addr::value, {0x9ABCDE, 0x9ABCDE});

    auto const result
      = apply(write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
              read(ThirdReg::mode),
              write(SecondReg::data, Kvasir::Register::value<std::uint32_t, 0x11>()),
              read(SimpleTestReg::dat),
              write(ThirdReg::config, Kvasir::Register::value<std::uint32_t, 0x22>()),
              read(SecondReg::status),
              write(SimpleTestReg::stop, ctv<stopT, 1>()));

    CHECK_EQ(result[ThirdReg::mode], 0x9AU);
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[SecondReg::status], 0x56U);

    // per register: the explicit read first, then a single merged write (cmd and stop
    // merge even though the dat read sits between them in the argument list)
    checkActions({
      R{     ThirdReg::Addr::value, 0x9ABCDE},
      R{     ThirdReg::Addr::value, 0x9ABCDE},
      W{     ThirdReg::Addr::value,   0x22DE},
      R{    SecondReg::Addr::value,   0x5678},
      W{    SecondReg::Addr::value,     0x11},
      R{SimpleTestReg::Addr::value,     0xAB},
      R{SimpleTestReg::Addr::value,     0xAB},
      W{SimpleTestReg::Addr::value,    0x3AB}
    });
}

int main() {
    writesToTwoRegisters();
    mixedReadsAndWritesTwoRegisters();
    threeRegisterMixedReadWrite();
    mixedWriteTypesOrder1();
    mixedWriteTypesOrder2();
    mixedWriteTypesOrder3();
    allRuntimeWritesWithReads();
    allCompileTimeWritesWithReads();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
