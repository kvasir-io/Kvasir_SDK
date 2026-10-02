// Tests for the register model of the test recorder (Recorder::Model): a modelled address keeps
// its value across accesses and behaves like the masks and hooks say, while the recorded trace
// stays the same as for an unmodelled one.
#include "test_registers.hpp"

#include <deque>

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using R = Recorder::Read;
using W = Recorder::Write;

template<typename T,
         unsigned V>
constexpr auto ctv() {
    return Kvasir::Register::value<T, static_cast<T>(V)>();
}

using divT  = typename decltype(CtrlReg::div)::DataType;
using dataT = typename decltype(DataReg::data)::DataType;

// a plain register remembers what was written and reads it back
static void plainWriteThenRead() {
    test("plainWriteThenRead");

    recorder.model(CtrlReg::Addr::value);

    apply(write(CtrlReg::div, ctv<divT, 0x5A>()));

    CHECK_EQ(recorder.model(CtrlReg::Addr::value).value, 0x5A0U);
    CHECK_EQ(static_cast<unsigned>(apply(read(CtrlReg::div))), 0x5AU);
}

// the three cases of a write-one-to-clear flag
static void oneToClearFlag() {
    test("oneToClearFlag");

    auto& r = recorder.model(CtrlReg::Addr::value);
    r.value = 0x0B;   // en | irq | flag
    r.oneToClear(0x08);

    apply(reset(CtrlReg::flag));   // read 0x0B, write 0x0B | 0x08: the flag is cleared
    CHECK_EQ(r.value, 0x03U);
    CHECK_EQ(static_cast<unsigned>(apply(read(CtrlReg::flag))), 0U);

    r.value = 0x0B;
    apply(clear(CtrlReg::irq));   // the RMW writes the flag's 1 back: the flag goes with it
    CHECK_EQ(r.value, 0x01U);

    r.value = 0x03;
    apply(reset(CtrlReg::flag));   // a one on a clear flag leaves it clear
    CHECK_EQ(r.value, 0x03U);
}

// one-to-set, one-to-toggle: written ones act, written zeros leave the bit alone
static void oneToSetAndToggle() {
    test("oneToSetAndToggle");

    auto& r = recorder.model(ToggleReg::Addr::value);
    r.oneToToggle(0xFFFFFFFF);
    r.value = 1U << 6;

    apply(clear(ToggleReg::pin6));   // writes the bit's current value back: 1 toggles it to 0
    CHECK_EQ(r.value, 0U);

    apply(clear(ToggleReg::pin6));   // already 0: writes 0, stays 0
    CHECK_EQ(r.value, 0U);

    auto& s = recorder.model(StatusReg::Addr::value);
    s.oneToSet(0x0F);
    s.value = 0x10;
    apply(write(StatusReg::all, runtimeValue(0x23)));
    // bits 3..0: a written one sets (bit 0, bit 1), bits 31..4 take the value (0x20)
    CHECK_EQ(s.value, 0x23U);
    apply(write(StatusReg::all, runtimeValue(0x00)));
    CHECK_EQ(s.value, 0x03U);
}

// strobes are never stored, read-only bits keep their value
static void selfClearingAndReadOnly() {
    test("selfClearingAndReadOnly");

    auto& r = recorder.model(StatusReg::Addr::value);
    r.readOnly(0x01).selfClearing(0x100);
    r.value = 0x01;

    apply(write(StatusReg::all, runtimeValue(0x1F0)));
    CHECK_EQ(r.value, 0xF1U);   // bit 0 kept, bit 8 not stored, the rest written
    CHECK_EQ(static_cast<unsigned>(apply(read(StatusReg::all))), 0xF1U);

    apply(write(StatusReg::all, runtimeValue(0x0)));
    CHECK_EQ(r.value, 0x01U);
}

// a clear-on-read bit is returned once, then gone
static void statusClearsOnRead() {
    test("statusClearsOnRead");

    auto& r = recorder.model(StatusReg::Addr::value);
    r.value = 0x05;
    r.clearOnRead(0x04);

    CHECK_EQ(static_cast<unsigned>(apply(read(StatusReg::all))), 0x05U);
    CHECK_EQ(static_cast<unsigned>(apply(read(StatusReg::all))), 0x01U);
    CHECK_EQ(static_cast<unsigned>(apply(read(StatusReg::overflow))), 0U);
}

// full control through the hooks: a FIFO
static void fifoPopsOnRead() {
    test("fifoPopsOnRead");

    std::deque<unsigned> fifo{0x11, 0x22};
    auto&                r = recorder.model(DataReg::Addr::value);
    r.onRead               = [&](unsigned) {
        if(fifo.empty()) { return 0U; }
        auto const v = fifo.front();
        fifo.pop_front();
        return v;
    };
    r.onWrite = [&](unsigned stored, unsigned written) {
        fifo.push_back(written);
        return stored;
    };

    CHECK_EQ(static_cast<unsigned>(apply(read(DataReg::data))), 0x11U);
    apply(write(DataReg::data, ctv<dataT, 0x33>()));
    CHECK_EQ(static_cast<unsigned>(apply(read(DataReg::data))), 0x22U);
    CHECK_EQ(static_cast<unsigned>(apply(read(DataReg::data))), 0x33U);
    CHECK(fifo.empty());
}

// a partial write to a register whose read has a side effect: the read-modify-write pops the
// FIFO (a compile error for generated registers)
static void partialWriteOnFifoPops() {
    test("partialWriteOnFifoPops");

    std::deque<unsigned> fifo{0x11, 0x22};
    auto&                r = recorder.model(DataReg::Addr::value);
    r.onRead               = [&](unsigned) {
        auto const v = fifo.front();
        fifo.pop_front();
        return v;
    };
    r.onWrite = [&](unsigned stored, unsigned written) {
        fifo.push_back(written);
        return stored;
    };

    apply(write(DataReg::low, ctv<dataT, 0x33>()));

    checkActionKinds("rw");
    CHECK_EQ(fifo.size(), 2U);
    CHECK_EQ(fifo.front(), 0x22U);   // 0x11 was read away by the RMW
    CHECK_EQ(fifo.back(), 0x33U);
}

// a model on one address leaves every other address on the queue, and the trace records both
static void modelledAndQueuedTogether() {
    test("modelledAndQueuedTogether");

    recorder.model(StatusReg::Addr::value).value = 0x04;
    recorder.setReadValue(CtrlReg::Addr::value, 0xF0);

    auto const overflow = static_cast<unsigned>(apply(read(StatusReg::overflow)));
    apply(set(CtrlReg::en));

    CHECK_EQ(overflow, 1U);
    checkActions({
      R{StatusReg::Addr::value, 0x04},
      R{  CtrlReg::Addr::value, 0xF0},
      W{  CtrlReg::Addr::value, 0xF1}
    });
    CHECK_EQ(readCount(StatusReg::Addr::value), 1U);
}

// a new test case starts without models
static void resetClearsModels() {
    test("resetClearsModels");

    CHECK(recorder.models.empty());
    CHECK_EQ(static_cast<unsigned>(apply(read(StatusReg::all))), 0U);
}

int main() {
    plainWriteThenRead();
    oneToClearFlag();
    oneToSetAndToggle();
    selfClearingAndReadOnly();
    statusClearsOnRead();
    fifoPopsOnRead();
    partialWriteOnFifoPops();
    modelledAndQueuedTogether();
    resetClearsModels();

    return report();
}
