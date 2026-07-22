// Tests for sequencePoint: it must prevent merging of reads and writes across the
// sequence point while keeping the groups in order.
#include "test_registers.hpp"

#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

// without the sequence point these two writes would merge into a single write
static void preventsWriteMerge() {
    test("preventsWriteMerge");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);

    apply(write(SimpleTestReg::dat, runtimeValue(0x11)),
          sequencePoint,
          write(SimpleTestReg::cmd, value<std::uint32_t, 1>()));

    // group 1: RMW keeps cmd/stop from the read, group 2: the injected value is used up,
    // so its RMW read returns 0
    checkActions({
      R{SimpleTestReg::Addr::value, 0x1FF},
      W{SimpleTestReg::Addr::value, 0x111},
      R{SimpleTestReg::Addr::value,     0},
      W{SimpleTestReg::Addr::value, 0x100}
    });
}

static void multipleSequencePoints() {
    test("multipleSequencePoints");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);
    recorder.setReadValue(ThirdReg::Addr::value, 0xAABBCC);

    apply(write(SimpleTestReg::dat, runtimeValue(0x11)),
          sequencePoint,
          write(SecondReg::data, runtimeValue(0x22)),
          sequencePoint,
          write(ThirdReg::control, runtimeValue(0x33)));

    // SimpleTestReg and ThirdReg need RMW, SecondReg does not
    checkActions({
      R{SimpleTestReg::Addr::value,    0x1FF},
      W{SimpleTestReg::Addr::value,    0x111},
      W{    SecondReg::Addr::value,     0x22},
      R{     ThirdReg::Addr::value, 0xAABBCC},
      W{     ThirdReg::Addr::value,   0xBB33}
    });
}

// reads from the same register do not merge across a sequence point
static void readsDoNotMergeAcross() {
    test("readsDoNotMergeAcross");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1AB);

    apply(read(SimpleTestReg::dat), sequencePoint, read(SimpleTestReg::dat));

    checkActions({
      R{SimpleTestReg::Addr::value, 0x1AB},
      R{SimpleTestReg::Addr::value,     0}
    });
}

// write before, read after: the RMW pair must complete before the second group's read
static void preservesOrder() {
    test("preservesOrder");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);

    apply(write(SimpleTestReg::dat, runtimeValue(0x42)), sequencePoint, read(SimpleTestReg::dat));

    checkActions({
      R{SimpleTestReg::Addr::value, 0x1FF},
      W{SimpleTestReg::Addr::value, 0x142},
      R{SimpleTestReg::Addr::value,     0}
    });
}

// both groups RMW the same register
static void rmwInBothGroups() {
    test("rmwInBothGroups");

    recorder.setReadValue(ThirdReg::Addr::value, 0xAABBCC);

    apply(write(ThirdReg::mode, runtimeValue(0x11)),
          sequencePoint,
          write(ThirdReg::control, runtimeValue(0x22)));

    checkActions({
      R{ThirdReg::Addr::value, 0xAABBCC},
      W{ThirdReg::Addr::value, 0x11BBCC},
      R{ThirdReg::Addr::value,        0},
      W{ThirdReg::Addr::value,     0x22}
    });
}

// compile-time write and runtime write separated by a sequence point both RMW
static void compileTimeThenRuntime() {
    test("compileTimeThenRuntime");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);

    apply(write(SimpleTestReg::cmd, value<std::uint32_t, 1>()),
          sequencePoint,
          write(SimpleTestReg::dat, runtimeValue(0x42)));

    checkActions({
      R{SimpleTestReg::Addr::value, 0x1FF},
      W{SimpleTestReg::Addr::value, 0x1FF},
      R{SimpleTestReg::Addr::value,     0},
      W{SimpleTestReg::Addr::value,  0x42}
    });
}

// sequence points at the start and end produce empty groups which are ignored
static void emptyGroups() {
    test("emptyGroups");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1FF);

    apply(sequencePoint, write(SimpleTestReg::dat, runtimeValue(0x11)), sequencePoint);

    checkActions({
      R{SimpleTestReg::Addr::value, 0x1FF},
      W{SimpleTestReg::Addr::value, 0x111}
    });
}

// several groups of reads: merging happens within a group, never across groups; the
// FieldTuple indexing still finds every field
static void multipleReadGroupsWithIndexing() {
    test("multipleReadGroupsWithIndexing");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1AB);
    recorder.setReadValue(SecondReg::Addr::value, 0xCD12);
    recorder.setReadValue(ThirdReg::Addr::value, 0x345678);

    auto const result = apply(read(SimpleTestReg::dat),
                              read(SecondReg::data),
                              sequencePoint,
                              read(ThirdReg::control),
                              read(SimpleTestReg::dat),
                              sequencePoint,
                              read(SecondReg::status));

    // group 1: SimpleTestReg + SecondReg, group 2: ThirdReg + SimpleTestReg again,
    // group 3: SecondReg again -> 5 reads total
    CHECK(recorder.actions.size() == 5);
    CHECK(readCount(SimpleTestReg::Addr::value) == 2);
    CHECK(readCount(SecondReg::Addr::value) == 2);
    CHECK(readCount(ThirdReg::Addr::value) == 1);

    // field indexing returns the values of the first read of each register
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[SecondReg::data], 0x12U);
    CHECK_EQ(result[SecondReg::status], 0xCDU);
    CHECK_EQ(result[ThirdReg::control], 0x78U);
}

// the same field read three times returns the injected values in sequence
static void sameRegisterReadThreeTimes() {
    test("sameRegisterReadThreeTimes");

    recorder.setReadValues(SimpleTestReg::Addr::value, {0x1AA, 0x1BB, 0x1CC});

    apply(read(SimpleTestReg::dat),
          sequencePoint,
          read(SimpleTestReg::dat),
          sequencePoint,
          read(SimpleTestReg::dat));

    checkActions({
      R{SimpleTestReg::Addr::value, 0x1AA},
      R{SimpleTestReg::Addr::value, 0x1BB},
      R{SimpleTestReg::Addr::value, 0x1CC}
    });
}

// different fields of the same register separated by a sequence point: two reads, both
// field values indexable
static void differentFieldsSameRegister() {
    test("differentFieldsSameRegister");

    recorder.setReadValue(SecondReg::Addr::value, 0xABCD);

    auto const result = apply(read(SecondReg::data), sequencePoint, read(SecondReg::status));

    checkActions({
      R{SecondReg::Addr::value, 0xABCD},
      R{SecondReg::Addr::value,      0}
    });
    CHECK_EQ(result[SecondReg::data], 0xCDU);
    CHECK_EQ(result[SecondReg::status], 0xABU);
}

// numeric get<>() indexing addresses each read individually
static void numericIndexing() {
    test("numericIndexing");

    recorder.setReadValues(SecondReg::Addr::value, {0x11CD, 0x22CD, 0x33CD});

    auto const result = apply(read(SecondReg::data),
                              sequencePoint,
                              read(SecondReg::data),
                              sequencePoint,
                              read(SecondReg::data));

    checkActions({
      R{SecondReg::Addr::value, 0x11CD},
      R{SecondReg::Addr::value, 0x22CD},
      R{SecondReg::Addr::value, 0x33CD}
    });

    CHECK_EQ(result.template get<0>(), 0xCDU);
    CHECK_EQ(result.template get<1>(), 0xCDU);
    CHECK_EQ(result.template get<2>(), 0xCDU);
    CHECK_EQ(result[SecondReg::data], 0xCDU);
}

int main() {
    preventsWriteMerge();
    multipleSequencePoints();
    readsDoNotMergeAcross();
    preservesOrder();
    rmwInBothGroups();
    compileTimeThenRuntime();
    emptyGroups();
    multipleReadGroupsWithIndexing();
    sameRegisterReadThreeTimes();
    differentFieldsSameRegister();
    numericIndexing();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
