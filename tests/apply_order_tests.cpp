// apply()'s execution order (Register/Apply.hpp, the comment at the top): per step, register address descending,
// then reads before toggles before writes, actions of equal (address, rank) in argument order after flattening;
// steps in order; merges only between neighbours of one Address type.
#include "test_registers.hpp"

#include <deque>
#include <print>
#include <utility>

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace {
// the addresses of the recorded writes, in order
std::vector<unsigned> writeAddresses() {
    std::vector<unsigned> out;
    for(auto const& action : recorder.actions) {
        if(auto const* w = std::get_if<Recorder::Write>(&action)) { out.push_back(w->address); }
    }
    return out;
}

void checkWriteAddresses(std::vector<unsigned> const& expected,
                         std::source_location         loc = std::source_location::current()) {
    if(writeAddresses() != expected) {
        failf(loc, "write order differs");
        printRecordedActions();
    }
}

// three registers in one step, written in ascending address order: stored highest address first
void addressDescending() {
    test("addressDescending");
    apply(write(SimpleTestReg::dat, value<std::uint32_t, 1>()),
          write(SecondReg::data, value<std::uint32_t, 2>()),
          write(ThirdReg::control, value<std::uint32_t, 3>()));
    checkWriteAddresses(
      {ThirdReg::Addr::value, SecondReg::Addr::value, SimpleTestReg::Addr::value});
}

// one register: the read first, then the toggle, then the write, whatever the argument order
void readThenToggleThenWrite() {
    test("readThenToggleThenWrite");
    recorder.setReadValues(CtrlReg::Addr::value, {0x1, 0x1, 0x3});

    auto const r = apply(write(CtrlReg::div, value<std::uint32_t, 5>()),
                         toggle(CtrlReg::irq),
                         read(CtrlReg::en));

    CHECK_EQ(static_cast<unsigned>(r), 1U);
    checkActions({
      R{CtrlReg::Addr::value,  0x1},
      R{CtrlReg::Addr::value,  0x1},
      W{CtrlReg::Addr::value,  0x3},
      R{CtrlReg::Addr::value,  0x3},
      W{CtrlReg::Addr::value, 0x53}
    });
}

// equal (address, rank) that do not merge: argument order
void tiesInArgumentOrder() {
    test("tiesInArgumentOrder");
    recorder.setReadValues(CtrlReg::Addr::value, {0x0, 0x0, 0x0});

    apply(set(CtrlReg::en),
          atomic(set(CtrlReg::irq)),
          write(CtrlReg::div, value<std::uint32_t, 3>()));

    checkActions({
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value,  0x1},
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value,  0x2},
      R{CtrlReg::Addr::value,  0x0},
      W{CtrlReg::Addr::value, 0x30}
    });
}

// a nested list flattens in place: its actions merge with the top-level ones
void nestedListFlattens() {
    test("nestedListFlattens");
    recorder.setReadValue(CtrlReg::Addr::value, 0x0);

    apply(Kvasir::MPL::list(set(CtrlReg::en),
                            Kvasir::MPL::list(write(SecondReg::data, value<std::uint32_t, 7>()))),
          set(CtrlReg::irq));

    checkActions({
      R{  CtrlReg::Addr::value, 0x0},
      W{  CtrlReg::Addr::value, 0x3},
      W{SecondReg::Addr::value, 0x7}
    });
}

// steps run in order, whatever their addresses
void stepsInOrder() {
    test("stepsInOrder");
    apply(write(SimpleTestReg::dat, value<std::uint32_t, 1>()),
          sequencePoint,
          write(SecondReg::data, value<std::uint32_t, 2>()),
          sequencePoint,
          sequencePoint,
          write(ThirdReg::control, value<std::uint32_t, 3>()));
    checkWriteAddresses(
      {SimpleTestReg::Addr::value, SecondReg::Addr::value, ThirdReg::Addr::value});
}

// a second Address type with CtrlReg's number: two accesses, in argument order
struct TwinOfCtrl;
using TwinAddr = Address<CtrlReg::Addr::value, 0, 0, std::uint32_t, Mode<{}, TwinOfCtrl>>;
constexpr FieldLocation<TwinAddr, maskFromRange(2, 2), ReadWriteAccess, std::uint32_t> twinBit{};

void twoAddressTypesStayApart() {
    test("twoAddressTypesStayApart");
    recorder.setReadValues(CtrlReg::Addr::value, {0x0, 0x0});

    apply(set(CtrlReg::en), set(twinBit));

    checkActions({
      R{CtrlReg::Addr::value, 0x0},
      W{CtrlReg::Addr::value, 0x1},
      R{CtrlReg::Addr::value, 0x0},
      W{CtrlReg::Addr::value, 0x4}
    });
}

// 20 literal writes of one field, each followed by an atomic set: 40 actions of one rank on one register, none
// merges (a literal and an atomic write never do, two literals are never neighbours) - argument order throughout
template<std::size_t... I>
void longStep(std::index_sequence<I...>) {
    recorder.setReadValues(CtrlReg::Addr::value, std::deque<unsigned>(2 * sizeof...(I), 0x0));
    apply(Kvasir::MPL::list(write(CtrlReg::div, value<std::uint32_t, I>()),
                            atomic(set(CtrlReg::irq)))...);
    std::vector<Recorder::Action> expected;
    for(unsigned i = 0; i < sizeof...(I); ++i) {
        expected.emplace_back(R{CtrlReg::Addr::value, 0x0});
        expected.emplace_back(W{CtrlReg::Addr::value, i << 4});
        expected.emplace_back(R{CtrlReg::Addr::value, 0x0});
        expected.emplace_back(W{CtrlReg::Addr::value, 0x2});
    }
    checkActions(expected);
}

void longStepsKeepArgumentOrder() {
    test("longStepsKeepArgumentOrder 9");
    longStep(std::make_index_sequence<9>{});
    test("longStepsKeepArgumentOrder 20");
    longStep(std::make_index_sequence<20>{});
}

// three runtime writes of disjoint fields and a literal: one store, every input in it
void runtimeInputsMerge() {
    test("runtimeInputsMerge");
    apply(write(ThirdReg::control, runtimeValue(0x11)),
          write(ThirdReg::mode, value<std::uint32_t, 0x33>()),
          write(SecondReg::data, runtimeValue(0x44)),
          write(ThirdReg::config, runtimeValue(0x22)));
    checkActions({
      W{ ThirdReg::Addr::value, 0x332211},
      W{SecondReg::Addr::value,     0x44}
    });
}
}   // namespace

int main() {
    addressDescending();
    readThenToggleThenWrite();
    tiesInArgumentOrder();
    nestedListFlattens();
    stepsInOrder();
    twoAddressTypesStayApart();
    longStepsKeepArgumentOrder();
    runtimeInputsMerge();
    return Kvasir::Test::report();
}
