// Tests for the write paths of Kvasir::Register::apply: runtime, compile-time and mixed
// writes, merging of writes to the same register and R::overrideDefaults (its type list and its bundle).
#include "test_registers.hpp"

#include <cstdint>
#include <print>
#include <type_traits>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// all three fields written in one apply cover every non-write-ignored bit, so the whole
// register is written exactly once and no read-modify-write read is needed
template<typename Reg,
         bool Order>
void simpleWrite() {
    testFor<Reg>(Order ? "simpleWrite<dat,cmd,stop>" : "simpleWrite<stop,dat,cmd>");

    using CmdT  = typename decltype(Reg::cmd)::DataType;
    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(37);

    if constexpr(Order) {
        apply(write(Reg::dat, v),
              write(Reg::cmd, ctv<CmdT, 0>()),
              write(Reg::stop, ctv<StopT, 1>()));
    } else {
        apply(write(Reg::stop, ctv<StopT, 1>()),
              write(Reg::dat, v),
              write(Reg::cmd, ctv<CmdT, 0>()));
    }

    checkActions({
      W{Reg::Addr::value, v | 0x200}
    });
}

// R::overrideDefaults with a runtime value is a bundle apply() takes alone: it fills in the unmentioned fields from the
// defaults, the result is still a single merged write
template<typename Reg,
         bool Order>
void overrideDefaultsWrites() {
    testFor<Reg>(Order ? "overrideDefaultsWrites<dat,stop>" : "overrideDefaultsWrites<stop,dat>");

    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(123);

    if constexpr(Order) {
        apply(Reg::overrideDefaults(write(Reg::dat, v), write(Reg::stop, ctv<StopT, 1>())));
    } else {
        apply(Reg::overrideDefaults(write(Reg::stop, ctv<StopT, 1>()), write(Reg::dat, v)));
    }

    checkActions({
      W{Reg::Addr::value, v | 0x200}
    });
}

template<typename T>
inline constexpr bool isTypeList = false;

template<typename... Ts>
inline constexpr bool isTypeList<brigand::list<Ts...>> = true;

// with literal actions only R::overrideDefaults is a type list (a Startup list may hold it); apply() runs it as any
// list
template<typename Reg>
void overrideDefaultsLiterals() {
    testFor<Reg>("overrideDefaultsLiterals");

    using StopT = typename decltype(Reg::stop)::DataType;
    using DatT  = typename decltype(Reg::dat)::DataType;

    auto const l
      = Reg::overrideDefaults(write(Reg::stop, ctv<StopT, 1>()), write(Reg::dat, ctv<DatT, 5>()));
    static_assert(isTypeList<std::remove_const_t<decltype(l)>>);
    apply(l);

    checkActions({
      W{Reg::Addr::value, 0x205}
    });
}

// the bundle holds the given actions and, as its Defaults, the default writes of the fields they leave out
static_assert(std::is_same_v<decltype(SimpleTestReg::overrideDefaults(write(SimpleTestReg::dat,
                                                                            runtimeValue(1U)))),
                             Defaulted<brigand::list<decltype(write(SimpleTestReg::stop,
                                                                    value<std::uint32_t,
                                                                          0>())),
                                                     decltype(write(SimpleTestReg::cmd,
                                                                    value<std::uint32_t,
                                                                          0>()))>,
                                       decltype(write(SimpleTestReg::dat,
                                                      runtimeValue(1U)))>>);

template<typename Reg>
void runtimeOnlyWrites() {
    testFor<Reg>("runtimeOnlyWrites");

    using CmdT  = typename decltype(Reg::cmd)::DataType;
    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v1 = runtimeValue(0x5A);
    auto const v2 = runtimeValue(1);

    apply(write(Reg::dat, v1),
          write(Reg::cmd, static_cast<CmdT>(v2)),
          write(Reg::stop, static_cast<StopT>(1)));

    checkActions({
      W{Reg::Addr::value, v1 | (v2 << 8) | (1U << 9)}
    });
}

template<typename Reg>
void compileTimeOnlyWrites() {
    testFor<Reg>("compileTimeOnlyWrites");

    using CmdT  = typename decltype(Reg::cmd)::DataType;
    using StopT = typename decltype(Reg::stop)::DataType;

    apply(write(Reg::dat, Kvasir::Register::value<std::uint32_t, 42>()),
          write(Reg::cmd, ctv<CmdT, 0>()),
          write(Reg::stop, ctv<StopT, 1>()));

    checkActions({
      W{Reg::Addr::value, 42 | (1U << 9)}
    });
}

template<typename Reg>
void threeRuntimeWrites() {
    testFor<Reg>("threeRuntimeWrites");

    using CmdT  = typename decltype(Reg::cmd)::DataType;
    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v1 = runtimeValue(0xA5);
    auto const v2 = runtimeValue(1);
    auto const v3 = runtimeValue(1);

    apply(write(Reg::dat, v1),
          write(Reg::cmd, static_cast<CmdT>(v2)),
          write(Reg::stop, static_cast<StopT>(v3)));

    checkActions({
      W{Reg::Addr::value, v1 | (v2 << 8) | (v3 << 9)}
    });
}

// runtime, compile-time, compile-time pattern
template<typename Reg>
void alternatingWrites() {
    testFor<Reg>("alternatingWrites");

    using CmdT  = typename decltype(Reg::cmd)::DataType;
    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x33);

    apply(write(Reg::dat, v), write(Reg::cmd, ctv<CmdT, 0>()), write(Reg::stop, ctv<StopT, 1>()));

    checkActions({
      W{Reg::Addr::value, v | (1U << 9)}
    });
}

// all-compile-time applies take the non-indexed path and must still merge fields of the
// same register into a single write
void allCompileTimeTwoFieldMerge() {
    test("allCompileTimeTwoFieldMerge");

    using DataT   = typename decltype(SecondReg::data)::DataType;
    using StatusT = typename decltype(SecondReg::status)::DataType;

    apply(write(SecondReg::data, ctv<DataT, 0xAB>()),
          write(SecondReg::status, ctv<StatusT, 0xCD>()));

    checkActions({
      W{SecondReg::Addr::value, 0xCDAB}
    });
}

void allCompileTimeThreeFieldMerge() {
    test("allCompileTimeThreeFieldMerge");

    using ControlT = typename decltype(ThirdReg::control)::DataType;
    using ConfigT  = typename decltype(ThirdReg::config)::DataType;
    using ModeT    = typename decltype(ThirdReg::mode)::DataType;

    apply(write(ThirdReg::control, ctv<ControlT, 0xAB>()),
          write(ThirdReg::config, ctv<ConfigT, 0xCD>()),
          write(ThirdReg::mode, ctv<ModeT, 0xEF>()));

    checkActions({
      W{ThirdReg::Addr::value, 0xEFCDAB}
    });
}

// FieldValue constants (enum typed) merge with a plain compile-time write
void fieldValueWritesMerge() {
    test("fieldValueWritesMerge");

    apply(write(ComplexTestReg::CMDValC::read),
          write(ComplexTestReg::STOPValC::enable),
          write(ComplexTestReg::dat, Kvasir::Register::value<std::uint32_t, 5>()));

    checkActions({
      W{ComplexTestReg::Addr::value, 5 | (1U << 8) | (1U << 9)}
    });
}

// (two runtime writes of one field with a literal between them are a compile error since the merge
// table carries the runtime bits: tests/diagnostics_must_fail, runtime_literal_runtime_same_bits)

// two runtime writes to two different fields of the same register merge into one write
void twoRuntimeWritesDifferentFieldsMerge() {
    test("twoRuntimeWritesDifferentFieldsMerge");

    using CmdT = typename decltype(SimpleTestReg::cmd)::DataType;

    auto const v1 = runtimeValue(0x11);
    auto const v2 = runtimeValue(1);

    apply(write(SimpleTestReg::dat, v1), write(SimpleTestReg::cmd, static_cast<CmdT>(v2)));

    CHECK(writeCount(SimpleTestReg::Addr::value) == 1);
    auto const written = writtenValue(SimpleTestReg::Addr::value);
    CHECK_EQ(written & 0xFFU, v1);
    CHECK_EQ((written >> 8) & 1U, v2);
}

// the same literal twice in one apply (two peripherals setting one bit, or a field written
// with one value from two places) is idempotent and merges into a single write
void agreeingLiteralsMerge() {
    test("agreeingLiteralsMerge");

    using StopT = typename decltype(SimpleTestReg::stop)::DataType;
    using CmdT  = typename decltype(SimpleTestReg::cmd)::DataType;

    apply(set(SimpleTestReg::stop),
          write(SimpleTestReg::dat, ctv<std::uint32_t, 0x5A>()),
          clear(SimpleTestReg::cmd),
          set(SimpleTestReg::stop),
          write(SimpleTestReg::stop, ctv<StopT, 1>()),
          write(SimpleTestReg::cmd, ctv<CmdT, 0>()),
          write(SimpleTestReg::dat, ctv<std::uint32_t, 0x5A>()));

    checkActions({
      W{SimpleTestReg::Addr::value, 0x5AU | (1U << 9)}
    });
}

// Overlapping writes in one apply() where a runtime value is involved
namespace OverlapRules {
    using Kvasir::Register::RegisterTraits;
    using Kvasir::Register::Detail::literalLeavesRuntimeBits;
    using Kvasir::Register::Detail::runtimeDisjoint;
    static_assert(runtimeDisjoint(0x1,
                                  0x2,
                                  RegisterTraits<CtrlReg::Addr>::writeIgnoredIfZeroMask),
                  "different fields: fine");
    static_assert(!runtimeDisjoint(0xF0,
                                   0x30,
                                   RegisterTraits<CtrlReg::Addr>::writeIgnoredIfZeroMask),
                  "two runtime writes of the same bits");
    static_assert(runtimeDisjoint(0x20,
                                  0x20,
                                  RegisterTraits<ToggleReg::Addr>::writeIgnoredIfZeroMask),
                  "a zero-ignored register ORs on purpose");
    static_assert(literalLeavesRuntimeBits(0x0,
                                           0xF0,
                                           RegisterTraits<CtrlReg::Addr>::writeIgnoredIfZeroMask),
                  "a 0 default under a runtime field");
    static_assert(!literalLeavesRuntimeBits(0x20,
                                            0xF0,
                                            RegisterTraits<CtrlReg::Addr>::writeIgnoredIfZeroMask),
                  "a literal 1 under a runtime field");
}   // namespace OverlapRules

}   // namespace

int main() {
    simpleWrite<SimpleTestReg, true>();
    simpleWrite<SimpleTestReg, false>();
    simpleWrite<ComplexTestReg, true>();
    simpleWrite<ComplexTestReg, false>();

    overrideDefaultsWrites<SimpleTestReg, true>();
    overrideDefaultsWrites<SimpleTestReg, false>();
    overrideDefaultsWrites<ComplexTestReg, true>();
    overrideDefaultsWrites<ComplexTestReg, false>();
    overrideDefaultsLiterals<SimpleTestReg>();
    overrideDefaultsLiterals<ComplexTestReg>();

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
    twoRuntimeWritesDifferentFieldsMerge();
    agreeingLiteralsMerge();

    return Kvasir::Test::report();
}
