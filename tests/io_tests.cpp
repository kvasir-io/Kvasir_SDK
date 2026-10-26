// Tests for the Kvasir::Io layer.
//
// Two halves:
//  * the Io::Detail traits in Io/Utility.hpp, which are pure compile time predicates over
//    pin locations and are checked with static_asserts;
//  * the pin factories in Io/PinFactories.hpp. Those all funnel through
//    Io::MakeAction<TAction, TPinLocation>, which a chip support file is expected to
//    specialise. This test provides a small fake chip file (a GPIO block with a direction,
//    an output, a toggle and an input register per port) and then drives the factories
//    through apply() against the register mock.
#include "kvasir/Io/Io.hpp"

#include "kvasir/Io/PinFactories.hpp"
#include "kvasir/Io/Types.hpp"
#include "kvasir/Io/Utility.hpp"
#include "kvasir_test.hpp"

#include <cstdint>
#include <type_traits>

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

template<int Port, int Pin>
using Loc = Kvasir::Io::PinLocation<Port, Pin>;

// the pin and port a location names, as values
static_assert(Kvasir::Io::pinNumber(Loc<0,
                                        25>{})
              == 25);
static_assert(Loc<1,
                  7>::pin
                == 7
              && Loc<1,
                     7>::port
                   == 1);

// ---------------------------------------------------------------------------
// fake chip file: one GPIO block per port
//
//   base + 0x0  direction   1 = output, plain read-write bits
//   base + 0x4  output      plain read-write bits
//   base + 0x8  toggle      write a one to flip a bit, writing zero is ignored
//   base + 0xC  input       read only
template<int Port>
constexpr unsigned portBase = 0x100U + static_cast<unsigned>(Port) * 0x10U;

template<int Port>
using DirAddr = Kvasir::Register::Address<portBase<Port>, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port>
using OutAddr
  = Kvasir::Register::Address<portBase<Port> + 4, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port>
using ToggleAddr
  = Kvasir::Register::Address<portBase<Port> + 8, 0xFFFFFFFF, 0x00000000, std::uint32_t>;

template<int Port>
using InAddr
  = Kvasir::Register::Address<portBase<Port> + 12, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port, int Pin>
using DirField = Kvasir::Register::FieldLocation<DirAddr<Port>,
                                                 (1U << static_cast<unsigned>(Pin)),
                                                 Kvasir::Register::ReadWriteAccess,
                                                 std::uint32_t>;

template<int Port, int Pin>
using OutField = Kvasir::Register::FieldLocation<OutAddr<Port>,
                                                 (1U << static_cast<unsigned>(Pin)),
                                                 Kvasir::Register::ReadWriteAccess,
                                                 std::uint32_t>;

using ToggleAccess
  = Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                             Kvasir::Register::ReadActionType::normal,
                             Kvasir::Register::ModifiedWriteValueType::oneToToggle>;

template<int Port, int Pin>
using ToggleField = Kvasir::Register::
  FieldLocation<ToggleAddr<Port>, (1U << static_cast<unsigned>(Pin)), ToggleAccess, std::uint32_t>;

template<int Port, int Pin>
using InField = Kvasir::Register::FieldLocation<InAddr<Port>,
                                                (1U << static_cast<unsigned>(Pin)),
                                                Kvasir::Register::ReadOnlyAccess,
                                                std::uint32_t>;

}   // namespace

// the chip file specialisations the pin factories resolve to
namespace Kvasir { namespace Io {

    template<PullConfiguration PC, int Port, int Pin>
    struct MakeAction<Action::Input<PC>, PinLocation<Port, Pin>>
      : Register::Detail::Clear<DirField<Port, Pin>> {};

    template<OutputType OT, OutputSpeed OS, int Port, int Pin>
    struct MakeAction<Action::Output<OT, OS, OutputInit::low>, PinLocation<Port, Pin>>
      : Register::Detail::Set<DirField<Port, Pin>> {};

    template<OutputType OT, OutputSpeed OS, int Port, int Pin>
    struct MakeAction<Action::Output<OT, OS, OutputInit::high>, PinLocation<Port, Pin>>
      : Register::Detail::Set<DirField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Set, PinLocation<Port, Pin>>
      : Register::Detail::Set<OutField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Clear, PinLocation<Port, Pin>>
      : Register::Detail::Clear<OutField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Toggle, PinLocation<Port, Pin>>
      : Register::Detail::Set<ToggleField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Read, PinLocation<Port, Pin>>
      : Register::Action<InField<Port, Pin>, Register::ReadAction> {};

}}   // namespace Kvasir::Io

using namespace Kvasir::Register;
using namespace Kvasir::Test;

namespace io     = Kvasir::Io;
namespace detail = Kvasir::Io::Detail;

// ===========================================================================
// Io::Detail traits
// ===========================================================================

// --- IsPinLocation --------------------------------------------------------
static_assert(detail::IsPinLocation<Loc<0,
                                        0>>::value);
static_assert(detail::IsPinLocation<Loc<3,
                                        17>>::value);
static_assert(!detail::IsPinLocation<int>::value);
static_assert(!detail::IsPinLocation<io::Pin<0>>::value);
static_assert(!detail::IsPinLocation<io::HwPort<0>>::value);

// --- PinLocationLess ------------------------------------------------------
// pins are ordered by port first, then by pin number
static_assert(detail::PinLocationLess<Loc<0,
                                          0>,
                                      Loc<0,
                                          1>>::value);
static_assert(!detail::PinLocationLess<Loc<0,
                                           1>,
                                       Loc<0,
                                           0>>::value);
static_assert(!detail::PinLocationLess<Loc<0,
                                           1>,
                                       Loc<0,
                                           1>>::value);
static_assert(detail::PinLocationLess<Loc<0,
                                          9>,
                                      Loc<1,
                                          0>>::value);
static_assert(!detail::PinLocationLess<Loc<1,
                                           0>,
                                       Loc<0,
                                           9>>::value);

// --- pinLocationEqual -----------------------------------------------------
static_assert(io::pinLocationEqual(Loc<0,
                                       0>{},
                                   Loc<0,
                                       0>{}));
static_assert(!io::pinLocationEqual(Loc<0,
                                        0>{},
                                    Loc<0,
                                        1>{}));
static_assert(!io::pinLocationEqual(Loc<0,
                                        0>{},
                                    Loc<1,
                                        0>{}));
// anything that is not a pair of pin locations compares unequal
static_assert(!io::pinLocationEqual(Loc<0,
                                        0>{},
                                    1));

// ===========================================================================
// makePinLocation
// ===========================================================================

static_assert(same<decltype(io::makePinLocation(io::HwPort<2>{},
                                                io::Pin<5>{})),
                   Loc<2,
                       5>>);
static_assert(same<decltype(io::makePinLocation(io::HwPort<0>{},
                                                io::Pin<0>{})),
                   Loc<0,
                       0>>);
// a port tag, then a pin tag: swapped, or a plain integral constant, it is not callable
template<typename P, typename N>
concept PinLocationOf = requires { io::makePinLocation(P{}, N{}); };
static_assert(PinLocationOf<io::HwPort<0>,
                            io::Pin<3>>);
static_assert(!PinLocationOf<io::Pin<3>,
                             io::HwPort<0>>);
static_assert(!PinLocationOf<std::integral_constant<int,
                                                    0>,
                             std::integral_constant<int,
                                                    3>>);

// ===========================================================================
// the pin factories live in Io; Register's factories take fields only
// ===========================================================================

// several pins: the list of what each pin alone gives (Io's variadic forms)
static_assert(same<decltype(io::read(Loc<0,
                                         2>{},
                                     Loc<1,
                                         7>{})),
                   brigand::list<decltype(io::read(Loc<0,
                                                       2>{})),
                                 decltype(io::read(Loc<1,
                                                       7>{}))>>);
static_assert(same<decltype(io::clear(Loc<0,
                                          2>{},
                                      Loc<0,
                                          3>{},
                                      Loc<1,
                                          4>{})),
                   brigand::list<decltype(io::clear(Loc<0,
                                                        2>{})),
                                 decltype(io::clear(Loc<0,
                                                        3>{})),
                                 decltype(io::clear(Loc<1,
                                                        4>{}))>>);
// found by argument-dependent lookup, unqualified, next to `using namespace Kvasir::Register`
static_assert(same<decltype(read(Loc<0,
                                     2>{},
                                 Loc<1,
                                     7>{})),
                   decltype(io::read(Loc<0,
                                         2>{},
                                     Loc<1,
                                         7>{}))>);

template<typename... Ts>
concept RegisterRead = requires { Kvasir::Register::read(Ts{}...); };
template<typename... Ts>
concept RegisterSet = requires { Kvasir::Register::set(Ts{}...); };
template<typename... Ts>
concept RegisterClear = requires { Kvasir::Register::clear(Ts{}...); };
template<typename... Ts>
concept IoSet = requires { io::set(Ts{}...); };

static_assert(!RegisterRead<Loc<0,
                                1>>);
static_assert(!RegisterRead<Loc<0,
                                1>,
                            Loc<0,
                                2>>);
static_assert(!RegisterClear<Loc<0,
                                 1>,
                             Loc<0,
                                 2>>);
static_assert(!RegisterSet<OutField<0,
                                    1>,
                           Loc<0,
                               2>>);   // a field and a pin in one call: neither family
static_assert(!IoSet<OutField<0,
                              1>>);   // Io's factories take pins only
static_assert(RegisterSet<OutField<0,
                                   1>,
                          OutField<0,
                                   2>>);
static_assert(RegisterRead<InField<0,
                                   1>,
                           InField<0,
                                   2>>);

// ===========================================================================
// the pin factories, driven through apply()
// ===========================================================================

namespace {
void makeOutputSetsDirectionBit() {
    test("makeOutputSetsDirectionBit");

    apply(makeOutput(Loc<0, 3>{}));

    // the direction register has no write-ignored bits, so this is a read-modify-write
    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0>), 1U << 3);
}

void makeInputClearsDirectionBit() {
    test("makeInputClearsDirectionBit");

    recorder.setReadValue(portBase<0>, 0xFFFFFFFF);
    apply(makeInput(Loc<0, 3>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0>), 0xFFFFFFF7U);
}

void makeOutputInitHigh() {
    test("makeOutputInitHigh");

    apply(makeOutputInitHigh(Loc<1, 2>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<1>), 1U << 2);
}

// the variadic form configures several pins at once; pins on the same port merge into a
// single register access
void variadicMakeOutputSamePort() {
    test("variadicMakeOutputSamePort");

    apply(makeOutput(Loc<0, 0>{}, Loc<0, 1>{}, Loc<0, 2>{}));

    checkActionKinds("rw");
    CHECK_EQ(writeCount(portBase<0>), 1U);
    CHECK_EQ(writtenValue(portBase<0>), 0x7U);
}

void variadicMakeOutputDifferentPorts() {
    test("variadicMakeOutputDifferentPorts");

    apply(makeOutput(Loc<0, 0>{}, Loc<1, 1>{}));

    // one read-modify-write per port
    CHECK_EQ(writeCount(portBase<0>), 1U);
    CHECK_EQ(writeCount(portBase<1>), 1U);
    CHECK_EQ(writtenValue(portBase<0>), 0x1U);
    CHECK_EQ(writtenValue(portBase<1>), 0x2U);
}

void variadicMakeInput() {
    test("variadicMakeInput");

    recorder.setReadValue(portBase<0>, 0xFFFFFFFF);
    apply(makeInput(Loc<0, 0>{}, Loc<0, 1>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0>), 0xFFFFFFFCU);
}

void setPin() {
    test("setPin");

    apply(set(Loc<0, 5>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0> + 4), 1U << 5);
}

void clearPin() {
    test("clearPin");

    recorder.setReadValue(portBase<0> + 4, 0xFFFFFFFF);
    apply(clear(Loc<0, 5>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0> + 4), 0xFFFFFFDFU);
}

void setAndClearDifferentPinsMerge() {
    test("setAndClearDifferentPinsMerge");

    recorder.setReadValue(portBase<0> + 4, 0x00000000);
    apply(set(Loc<0, 0>{}), clear(Loc<0, 1>{}));

    // both pins live in the same output register, so one read-modify-write covers them
    CHECK_EQ(writeCount(portBase<0> + 4), 1U);
    CHECK_EQ(writtenValue(portBase<0> + 4), 0x1U);
}

void togglePin() {
    test("togglePin");

    apply(toggle(Loc<0, 6>{}));

    // the toggle register is write-ignored-if-zero, so no read is needed
    checkActions({
      W{portBase<0> + 8, 1U << 6}
    });
}

void variadicToggle() {
    test("variadicToggle");

    apply(toggle(Loc<0, 0>{}, Loc<0, 1>{}, Loc<0, 2>{}));

    checkActions({
      W{portBase<0> + 8, 0x7U}
    });
}

void readPin() {
    test("readPin");

    recorder.setReadValue(portBase<0> + 12, 1U << 4);
    auto const results = apply(read(Loc<0, 4>{}));

    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);
}

void readPinLow() {
    test("readPinLow");

    recorder.setReadValue(portBase<0> + 12, 0x00000000);
    auto const results = apply(read(Loc<0, 4>{}));

    CHECK_EQ(get<0>(results), 0U);
}

void readTwoPinsSameRegister() {
    test("readTwoPinsSameRegister");

    recorder.setReadValue(portBase<0> + 12, (1U << 2) | (1U << 7));
    auto const results = apply(read(Loc<0, 2>{}), read(Loc<0, 7>{}));

    // both pins are in the same input register, one read serves both
    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);
    CHECK_EQ(get<1>(results), 1U);
}

// read(a, b) of two pins: Io's variadic read, one register read for both
void variadicReadPins() {
    test("variadicReadPins");

    recorder.setReadValue(portBase<0> + 12, 1U << 7);
    auto const results = apply(read(Loc<0, 2>{}, Loc<0, 7>{}));

    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 0U);
    CHECK_EQ(get<1>(results), 1U);
}

// clear(a, b) of two pins: Io's variadic clear, one read-modify-write of the output register
void variadicClearPins() {
    test("variadicClearPins");

    recorder.setReadValue(portBase<0> + 4, 0xFFFFFFFF);
    apply(clear(Loc<0, 1>{}, Loc<0, 4>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0> + 4), 0xFFFFFFEDU);
}

// the generic action() factory reaches the same chip specialisations
void genericActionFactory() {
    test("genericActionFactory");

    apply(action(io::Action::Set{}, Loc<0, 1>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(portBase<0> + 4), 1U << 1);
}

// configuring a pin and driving it in the same apply touches two different registers
void configureAndDriveInOneApply() {
    test("configureAndDriveInOneApply");

    apply(makeOutput(Loc<0, 3>{}), set(Loc<0, 3>{}));

    CHECK_EQ(writeCount(portBase<0>), 1U);       // direction
    CHECK_EQ(writeCount(portBase<0> + 4), 1U);   // output
    CHECK_EQ(writtenValue(portBase<0>), 1U << 3);
    CHECK_EQ(writtenValue(portBase<0> + 4), 1U << 3);
}
}   // namespace

int main() {
    makeOutputSetsDirectionBit();
    makeInputClearsDirectionBit();
    makeOutputInitHigh();
    variadicMakeOutputSamePort();
    variadicMakeOutputDifferentPorts();
    variadicMakeInput();

    setPin();
    clearPin();
    setAndClearDifferentPinsMerge();
    togglePin();
    variadicToggle();

    readPin();
    readPinLow();
    readTwoPinsSameRegister();
    variadicReadPins();
    variadicClearPins();

    genericActionFactory();
    configureAndDriveInOneApply();

    return Kvasir::Test::report();
}
