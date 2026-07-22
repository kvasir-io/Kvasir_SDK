// Tests for the read paths of Kvasir::Register::apply: read merging per register, value
// injection and the FieldTuple result indexing.
#include "test_registers.hpp"

#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

template<typename Reg>
static void singleRead() {
    test("singleRead");

    apply(read(Reg::dat));

    checkActions({
      R{Reg::Addr::value, 0}
    });
}

// multiple fields of the same register merge into a single read
template<typename Reg>
static void mergedReadsSameRegister() {
    test("mergedReadsSameRegister");

    apply(read(Reg::dat, Reg::cmd, Reg::stop));

    checkActions({
      R{Reg::Addr::value, 0}
    });
}

// an injected register value must show up masked and shifted in the field result
template<typename Reg>
static void readValueInjection() {
    test("readValueInjection");

    recorder.setReadValue(Reg::Addr::value, 0xABCD);

    auto const readValue = static_cast<unsigned>(apply(read(Reg::dat)));

    CHECK_EQ(readValue, 0xCDU);   // dat is bits 7..0
    checkActions({
      R{Reg::Addr::value, 0xABCD}
    });
}

template<typename Reg>
static void combinedReadsFromSameRegister() {
    test("combinedReadsFromSameRegister");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    recorder.setReadValue(Reg::Addr::value, 0x3AB);   // dat=0xAB, cmd=1, stop=1

    auto const result = apply(read(Reg::dat, Reg::cmd, Reg::stop));

    checkActions({
      R{Reg::Addr::value, 0x3AB}
    });
    CHECK_EQ(result[Reg::dat], 0xABU);
    CHECK_EQ(result[Reg::cmd], static_cast<cmdT>(1));
    CHECK_EQ(result[Reg::stop], static_cast<stopT>(1));
}

static void combinedReadsFromDifferentRegisters() {
    test("combinedReadsFromDifferentRegisters");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x1AB);
    recorder.setReadValue(SecondReg::Addr::value, 0xCD);

    auto const result = apply(read(SimpleTestReg::dat), read(SecondReg::data));

    CHECK(recorder.actions.size() == 2);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(SecondReg::Addr::value) == 1);
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[SecondReg::data], 0xCDU);
}

// two separate applies read two registers, each returning its injected value
static void sequentialReadsFromDifferentRegisters() {
    test("sequentialReadsFromDifferentRegisters");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0xAB);
    recorder.setReadValue(SecondReg::Addr::value, 0xCD);

    auto const val1 = static_cast<unsigned>(apply(read(SimpleTestReg::dat)));
    auto const val2 = static_cast<unsigned>(apply(read(SecondReg::data)));

    CHECK_EQ(val1, 0xABU);
    CHECK_EQ(val2, 0xCDU);
    checkActions({
      R{SimpleTestReg::Addr::value, 0xAB},
      R{    SecondReg::Addr::value, 0xCD}
    });
}

// multiple fields from multiple registers: one read per register, fields merged
static void complexCombinedReads() {
    test("complexCombinedReads");

    using cmdT  = typename decltype(SimpleTestReg::cmd)::DataType;
    using stopT = typename decltype(SimpleTestReg::stop)::DataType;

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x3FF);   // dat=0xFF, cmd=1, stop=1
    recorder.setReadValue(SecondReg::Addr::value, 0xABCD);      // data=0xCD, status=0xAB

    auto const result = apply(read(SimpleTestReg::dat),
                              read(SimpleTestReg::cmd),
                              read(SecondReg::data),
                              read(SecondReg::status),
                              read(SimpleTestReg::stop));

    CHECK(recorder.actions.size() == 2);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(SecondReg::Addr::value) == 1);
    CHECK_EQ(result[SimpleTestReg::dat], 0xFFU);
    CHECK_EQ(result[SimpleTestReg::cmd], static_cast<cmdT>(1));
    CHECK_EQ(result[SimpleTestReg::stop], static_cast<stopT>(1));
    CHECK_EQ(result[SecondReg::data], 0xCDU);
    CHECK_EQ(result[SecondReg::status], 0xABU);
}

static void threeRegisterSimpleReads() {
    test("threeRegisterSimpleReads");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x12);
    recorder.setReadValue(SecondReg::Addr::value, 0x34);
    recorder.setReadValue(ThirdReg::Addr::value, 0x56);

    auto const result
      = apply(read(SimpleTestReg::dat), read(SecondReg::data), read(ThirdReg::control));

    CHECK(recorder.actions.size() == 3);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(SecondReg::Addr::value) == 1);
    CHECK(readCount(ThirdReg::Addr::value) == 1);
    CHECK_EQ(result[SimpleTestReg::dat], 0x12U);
    CHECK_EQ(result[SecondReg::data], 0x34U);
    CHECK_EQ(result[ThirdReg::control], 0x56U);
}

static void threeRegisterMultipleFieldReads() {
    test("threeRegisterMultipleFieldReads");

    using cmdT  = typename decltype(SimpleTestReg::cmd)::DataType;
    using stopT = typename decltype(SimpleTestReg::stop)::DataType;

    recorder.setReadValue(SimpleTestReg::Addr::value, 0x3AB);   // dat=0xAB, cmd=1, stop=1
    recorder.setReadValue(SecondReg::Addr::value, 0xCDEF);      // data=0xEF, status=0xCD
    recorder.setReadValue(ThirdReg::Addr::value,
                          0x123456);   // control=0x56, config=0x34, mode=0x12

    auto const result = apply(read(SimpleTestReg::dat),
                              read(SecondReg::data),
                              read(SecondReg::status),
                              read(ThirdReg::control),
                              read(ThirdReg::config),
                              read(ThirdReg::mode),
                              read(SimpleTestReg::cmd),
                              read(SimpleTestReg::stop));

    CHECK(recorder.actions.size() == 3);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(SecondReg::Addr::value) == 1);
    CHECK(readCount(ThirdReg::Addr::value) == 1);
    CHECK_EQ(result[SimpleTestReg::dat], 0xABU);
    CHECK_EQ(result[SimpleTestReg::cmd], static_cast<cmdT>(1));
    CHECK_EQ(result[SimpleTestReg::stop], static_cast<stopT>(1));
    CHECK_EQ(result[SecondReg::data], 0xEFU);
    CHECK_EQ(result[SecondReg::status], 0xCDU);
    CHECK_EQ(result[ThirdReg::control], 0x56U);
    CHECK_EQ(result[ThirdReg::config], 0x34U);
    CHECK_EQ(result[ThirdReg::mode], 0x12U);
}

int main() {
    singleRead<SimpleTestReg>();
    singleRead<ComplexTestReg>();

    mergedReadsSameRegister<SimpleTestReg>();
    mergedReadsSameRegister<ComplexTestReg>();

    readValueInjection<SimpleTestReg>();
    readValueInjection<ComplexTestReg>();

    combinedReadsFromSameRegister<SimpleTestReg>();
    combinedReadsFromSameRegister<ComplexTestReg>();

    combinedReadsFromDifferentRegisters();
    sequentialReadsFromDifferentRegisters();
    complexCombinedReads();
    threeRegisterSimpleReads();
    threeRegisterMultipleFieldReads();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
