// Tests for the write paths of Kvasir::Register::apply: runtime, compile-time and mixed
// writes, merging of writes to the same register and overrideDefaultsRuntime.
#include "test_registers.hpp"

#include <cstdint>
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

// all three fields written in one apply cover every non-write-ignored bit, so the whole
// register is written exactly once and no read-modify-write read is needed
template<typename Reg,
         bool Order>
static void simpleWrite() {
    test(Order ? "simpleWrite<dat,cmd,stop>" : "simpleWrite<stop,dat,cmd>");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(37);

    if constexpr(Order) {
        apply(write(Reg::dat, v),
              write(Reg::cmd, ctv<cmdT, 0>()),
              write(Reg::stop, ctv<stopT, 1>()));
    } else {
        apply(write(Reg::stop, ctv<stopT, 1>()),
              write(Reg::dat, v),
              write(Reg::cmd, ctv<cmdT, 0>()));
    }

    checkActions({
      W{Reg::Addr::value, v | 0x200}
    });
}

// overrideDefaultsRuntime fills in the unmentioned fields from the defaults, the result
// is still a single merged write
template<typename Reg,
         bool Order>
static void overrideDefaultsWrites() {
    test(Order ? "overrideDefaultsWrites<dat,stop>" : "overrideDefaultsWrites<stop,dat>");

    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(123);

    if constexpr(Order) {
        Reg::overrideDefaultsRuntime(write(Reg::dat, v), write(Reg::stop, ctv<stopT, 1>()));
    } else {
        Reg::overrideDefaultsRuntime(write(Reg::stop, ctv<stopT, 1>()), write(Reg::dat, v));
    }

    checkActions({
      W{Reg::Addr::value, v | 0x200}
    });
}

template<typename Reg>
static void runtimeOnlyWrites() {
    test("runtimeOnlyWrites");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v1 = runtimeValue(0x5A);
    auto const v2 = runtimeValue(1);

    apply(write(Reg::dat, v1),
          write(Reg::cmd, static_cast<cmdT>(v2)),
          write(Reg::stop, static_cast<stopT>(1)));

    checkActions({
      W{Reg::Addr::value, v1 | (v2 << 8) | (1U << 9)}
    });
}

template<typename Reg>
static void compileTimeOnlyWrites() {
    test("compileTimeOnlyWrites");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    apply(write(Reg::dat, Kvasir::Register::value<std::uint32_t, 42>()),
          write(Reg::cmd, ctv<cmdT, 0>()),
          write(Reg::stop, ctv<stopT, 1>()));

    checkActions({
      W{Reg::Addr::value, 42 | (1U << 9)}
    });
}

template<typename Reg>
static void threeRuntimeWrites() {
    test("threeRuntimeWrites");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v1 = runtimeValue(0xA5);
    auto const v2 = runtimeValue(1);
    auto const v3 = runtimeValue(1);

    apply(write(Reg::dat, v1),
          write(Reg::cmd, static_cast<cmdT>(v2)),
          write(Reg::stop, static_cast<stopT>(v3)));

    checkActions({
      W{Reg::Addr::value, v1 | (v2 << 8) | (v3 << 9)}
    });
}

// runtime, compile-time, compile-time pattern
template<typename Reg>
static void alternatingWrites() {
    test("alternatingWrites");

    using cmdT  = typename decltype(Reg::cmd)::DataType;
    using stopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x33);

    apply(write(Reg::dat, v), write(Reg::cmd, ctv<cmdT, 0>()), write(Reg::stop, ctv<stopT, 1>()));

    checkActions({
      W{Reg::Addr::value, v | (1U << 9)}
    });
}

// all-compile-time applies take the non-indexed path and must still merge fields of the
// same register into a single write
static void allCompileTimeTwoFieldMerge() {
    test("allCompileTimeTwoFieldMerge");

    using dataT   = typename decltype(SecondReg::data)::DataType;
    using statusT = typename decltype(SecondReg::status)::DataType;

    apply(write(SecondReg::data, ctv<dataT, 0xAB>()),
          write(SecondReg::status, ctv<statusT, 0xCD>()));

    checkActions({
      W{SecondReg::Addr::value, 0xCDAB}
    });
}

static void allCompileTimeThreeFieldMerge() {
    test("allCompileTimeThreeFieldMerge");

    using controlT = typename decltype(ThirdReg::control)::DataType;
    using configT  = typename decltype(ThirdReg::config)::DataType;
    using modeT    = typename decltype(ThirdReg::mode)::DataType;

    apply(write(ThirdReg::control, ctv<controlT, 0xAB>()),
          write(ThirdReg::config, ctv<configT, 0xCD>()),
          write(ThirdReg::mode, ctv<modeT, 0xEF>()));

    checkActions({
      W{ThirdReg::Addr::value, 0xEFCDAB}
    });
}

// FieldValue constants (enum typed) merge with a plain compile-time write
static void fieldValueWritesMerge() {
    test("fieldValueWritesMerge");

    apply(write(ComplexTestReg::CMDValC::read),
          write(ComplexTestReg::STOPValC::enable),
          write(ComplexTestReg::dat, Kvasir::Register::value<std::uint32_t, 5>()));

    checkActions({
      W{ComplexTestReg::Addr::value, 5 | (1U << 8) | (1U << 9)}
    });
}

// two runtime writes to the same field plus a compile-time write in between: everything
// still merges into a single write, the runtime values are or-ed together
static void runtimeAndLiteralSameFieldMerge() {
    test("runtimeAndLiteralSameFieldMerge");

    using cmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    auto const v1 = runtimeValue(0x11);
    auto const v2 = runtimeValue(0x22);

    apply(write(SimpleTestReg::dat, v1),
          write(SimpleTestReg::cmd, ctv<cmdT, 1>()),
          write(SimpleTestReg::dat, v2));

    CHECK(writeCount(SimpleTestReg::Addr::value) == 1);
    auto const written = writtenValue(SimpleTestReg::Addr::value);
    CHECK_EQ(written & 0x100U, 0x100U);
    CHECK_EQ(written & 0xFFU, v1 | v2);
}

// two runtime writes to two different fields of the same register merge into one write
static void twoRuntimeWritesDifferentFieldsMerge() {
    test("twoRuntimeWritesDifferentFieldsMerge");

    using cmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    auto const v1 = runtimeValue(0x11);
    auto const v2 = runtimeValue(1);

    apply(write(SimpleTestReg::dat, v1), write(SimpleTestReg::cmd, static_cast<cmdT>(v2)));

    CHECK(writeCount(SimpleTestReg::Addr::value) == 1);
    auto const written = writtenValue(SimpleTestReg::Addr::value);
    CHECK_EQ(written & 0xFFU, v1);
    CHECK_EQ((written >> 8) & 1U, v2);
}

int main() {
    simpleWrite<SimpleTestReg, true>();
    simpleWrite<SimpleTestReg, false>();
    simpleWrite<ComplexTestReg, true>();
    simpleWrite<ComplexTestReg, false>();

    overrideDefaultsWrites<SimpleTestReg, true>();
    overrideDefaultsWrites<SimpleTestReg, false>();
    overrideDefaultsWrites<ComplexTestReg, true>();
    overrideDefaultsWrites<ComplexTestReg, false>();

    runtimeOnlyWrites<SimpleTestReg>();
    runtimeOnlyWrites<ComplexTestReg>();

    compileTimeOnlyWrites<SimpleTestReg>();
    compileTimeOnlyWrites<ComplexTestReg>();

    threeRuntimeWrites<SimpleTestReg>();
    threeRuntimeWrites<ComplexTestReg>();

    alternatingWrites<SimpleTestReg>();
    alternatingWrites<ComplexTestReg>();

    allCompileTimeTwoFieldMerge();
    allCompileTimeThreeFieldMerge();
    fieldValueWritesMerge();
    runtimeAndLiteralSameFieldMerge();
    twoRuntimeWritesDifferentFieldsMerge();

    if(Kvasir::Test::failures != 0) {
        std::print("{} checks failed\n", Kvasir::Test::failures);
        return 1;
    }
    return 0;
}
