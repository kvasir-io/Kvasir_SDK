// Tests for read-modify-write behavior: when a read is needed, which bits are preserved
// and how injected register contents merge with written fields.
#include "test_registers.hpp"

#include <print>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// an explicit read plus writes to the same register: the explicit read stays separate
// from the RMW read (cmd, bit 8, is not covered by the writes, so a RMW read is needed)
template<typename Reg>
void explicitReadPlusRmwWrite() {
    testFor<Reg>("explicitReadPlusRmwWrite");

    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x21);

    apply(read(Reg::dat), write(Reg::dat, v), write(Reg::stop, ctv<StopT, 1>()));

    checkActions({
      R{Reg::Addr::value,             0},
      R{Reg::Addr::value,             0},
      W{Reg::Addr::value, v | (1U << 9)}
    });
}

// RMW merges the injected register content into the written value: bit 8 comes from the
// read, dat and stop from the write actions
template<typename Reg>
void rmwMergesInjectedValue() {
    testFor<Reg>("rmwMergesInjectedValue");

    using StopT = typename decltype(Reg::stop)::DataType;

    auto const v = runtimeValue(0x44);

    recorder.setReadValue(Reg::Addr::value, 0x100);   // bit 8 set

    apply(write(Reg::dat, v), write(Reg::stop, ctv<StopT, 1>()));

    checkActions({
      R{Reg::Addr::value,                     0x100},
      W{Reg::Addr::value, v | (1U << 8) | (1U << 9)}
    });
}

// writing one field preserves the other readable bits of the register
void rmwPreservesUnwrittenBits() {
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
void noRmwWhenAllFieldsCovered() {
    test("noRmwWhenAllFieldsCovered");

    recorder.setReadValue(SecondReg::Addr::value, 0xCDAB);

    apply(write(SecondReg::data, runtimeValue(0x12)), write(SecondReg::status, runtimeValue(0x34)));

    checkActions({
      W{SecondReg::Addr::value, 0x3412}
    });
}

// writing non-adjacent fields preserves the field in between
void rmwNonAdjacentFields() {
    test("rmwNonAdjacentFields");

    recorder.setReadValue(ThirdReg::Addr::value, 0xCCBBAA);

    apply(write(ThirdReg::control, runtimeValue(0x11)), write(ThirdReg::mode, runtimeValue(0x33)));

    checkActions({
      R{ThirdReg::Addr::value, 0xCCBBAA},
      W{ThirdReg::Addr::value, 0x33BB11}
    });
}

// a compile-time and a runtime write to the same field or together
void rmwRuntimeMergesWithCompileTime() {
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
void rmwMultipleRegisters() {
    test("rmwMultipleRegisters");

    recorder.setReadValue(SimpleTestReg::Addr::value, 0xFFFF);
    recorder.setReadValue(ThirdReg::Addr::value, 0x555555);

    apply(write(SimpleTestReg::dat, runtimeValue(0x11)), write(ThirdReg::mode, runtimeValue(0x33)));

    CHECK(recorder.actions.size() == 4);
    CHECK(readCount(SimpleTestReg::Addr::value) == 1);
    CHECK(readCount(ThirdReg::Addr::value) == 1);
    // dat = 0x11, cmd/stop (bits 9..8) and the reserved bits 15..10 preserved from the read
    CHECK_EQ(writtenValue(SimpleTestReg::Addr::value), 0xFF11U);
    // mode = 0x33, control/config preserved from the read
    CHECK_EQ(writtenValue(ThirdReg::Addr::value), 0x335555U);
}

// runtime writes to a field outside the writable mask still perform RMW and preserve
// the rest of the register
void rmwPartialRegisterWrite() {
    test("rmwPartialRegisterWrite");

    recorder.setReadValue(ThirdReg::Addr::value, 0x123456);

    apply(write(ThirdReg::config, runtimeValue(0xAB)));

    // config = 0xAB, control and the reserved bits 31..16 preserved (write-ignored-if-zero, written back as read)
    checkActions({
      R{ThirdReg::Addr::value, 0x123456},
      W{ThirdReg::Addr::value, 0x12AB56}
    });
}

// compile-time writes to the same register merge into one write no matter where a
// read of that register appears in the argument list; the reads always execute first
void writeMergeIndependentOfReadPosition() {
    test("writeMergeIndependentOfReadPosition");

    using CmdT  = typename decltype(SimpleTestReg::cmd)::DataType;
    using StopT = typename decltype(SimpleTestReg::stop)::DataType;

    // explicit read + RMW read + one merged write, identical for every argument order
    std::vector<Kvasir::Test::Recorder::Action> const expected{
      R{SimpleTestReg::Addr::value,  0xAB},
      R{SimpleTestReg::Addr::value,  0xAB},
      W{SimpleTestReg::Addr::value, 0x3AB}
    };

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(write(SimpleTestReg::cmd, ctv<CmdT, 1>()),
          read(SimpleTestReg::dat),
          write(SimpleTestReg::stop, ctv<StopT, 1>()));
    checkActions(expected);

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(write(SimpleTestReg::cmd, ctv<CmdT, 1>()),
          write(SimpleTestReg::stop, ctv<StopT, 1>()),
          read(SimpleTestReg::dat));
    checkActions(expected);

    recorder.reset();
    recorder.setReadValues(SimpleTestReg::Addr::value, {0xAB, 0xAB});
    apply(read(SimpleTestReg::dat),
          write(SimpleTestReg::cmd, ctv<CmdT, 1>()),
          write(SimpleTestReg::stop, ctv<StopT, 1>()));
    checkActions(expected);
}

// A key register and a read-with-side-effect register refuse the RMW the
// executors would do; the check as a trait (a failing static_assert cannot be tested in this TU)
namespace RmwHazards {
    using Kvasir::Register::RegisterTraits;
    using Kvasir::Register::Detail::rmwAllowed;
    static_assert(rmwAllowed<RegisterTraits<CtrlReg::Addr>,
                             0x1>(),
                  "a normal register allows it");
    static_assert(!rmwAllowed<RegisterTraits<KeyReg::Addr>,
                              0x4>(),
                  "a write without the key may not read");
    static_assert(rmwAllowed<RegisterTraits<KeyReg::Addr>,
                             0xFFFF0004>(),
                  "with the key it may");
    static_assert(!rmwAllowed<RegisterTraits<FifoReg::Addr>,
                              0xFF>(),
                  "never on a read side effect");
    static_assert(!rmwAllowed<RegisterTraits<IcrstReg::Addr>,
                              0xF>(),
                  "an unclassified write-only bit must be named");
    static_assert(rmwAllowed<RegisterTraits<IcrstReg::Addr>,
                             0x80F>(),
                  "named in the same write it may");
}   // namespace RmwHazards

// a register with no readable field: one store of exactly the named bits, no read
void writeOnlyRegisterNeverRead() {
    test("writeOnlyRegisterNeverRead");
    recorder.setReadValue(SetResetReg::Addr::value, 0xFFFFFFFF);   // a read would bring these in
    apply(set(SetResetReg::bs5));
    checkActions({
      W{SetResetReg::Addr::value, 1U << 5}
    });
    test("writeOnlyRegisterNeverRead: two fields, one store");
    recorder.setReadValue(SetResetReg::Addr::value, 0xFFFFFFFF);
    apply(set(SetResetReg::br5), set(SetResetReg::bs5));
    checkActions({
      W{SetResetReg::Addr::value, (1U << 21) | (1U << 5)}
    });
}

// with the write-only bit named, the partial write reads the rest and writes the bit as given
void writeOnlyBitNamed() {
    test("writeOnlyBitNamed");
    recorder.setReadValue(IcrstReg::Addr::value, 0xA0);
    apply(write(IcrstReg::latency, Kvasir::Register::value<std::uint32_t, 5U>()),
          clear(IcrstReg::icrst));
    checkActions({
      R{IcrstReg::Addr::value, 0xA0},
      W{IcrstReg::Addr::value, 0xA5}
    });
}

// a key register written whole (key + field): one write, no read
void keyRegisterWrittenWhole() {
    test("keyRegisterWrittenWhole");
    using KeyT   = typename decltype(KeyReg::key)::DataType;
    using ResetT = typename decltype(KeyReg::reset)::DataType;
    apply(write(KeyReg::key, Kvasir::Register::value<KeyT, 0x05FAU>()),
          write(KeyReg::reset, Kvasir::Register::value<ResetT, 1U>()));
    checkActionKinds(
      "rw");   // bits 15..3, 1..0 are neither ignored nor written: read, key supplied
}

}   // namespace

int main() {
    keyRegisterWrittenWhole();
    writeOnlyRegisterNeverRead();
    writeOnlyBitNamed();
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

    return Kvasir::Test::report();
}
