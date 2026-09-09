// Tests for the Kvasir::Io layer.
//
// Two halves:
//  * the Io::Detail traits in Io/Utility.hpp, which are pure compile time predicates over
//    pin locations and ports and are checked with static_asserts;
//  * the pin factories in Io/PinFactories.hpp. Those all funnel through
//    Io::MakeAction<TAction, TPinLocation>, which a chip support file is expected to
//    specialise. This test provides a small fake chip file (a GPIO block with a direction,
//    an output, a toggle and an input register per port) and then drives the factories
//    through apply() against the register mock.
#include "kvasir/Io/Io.hpp"

#include "kvasir/Io/PinFactories.hpp"
#include "kvasir/Io/PortFactories.hpp"
#include "kvasir/Io/Types.hpp"
#include "kvasir/Io/Utility.hpp"
#include "kvasir_test.hpp"

#include <cstdint>
#include <type_traits>

namespace mpl = Kvasir::MPL;

namespace {

template<typename A, typename B>
constexpr bool same = std::is_same_v<A, B>;

template<int Port, int Pin>
using Loc = Kvasir::Register::PinLocation<Port, Pin>;

// ---------------------------------------------------------------------------
// fake chip file: one GPIO block per port
//
//   base + 0x0  direction   1 = output, plain read-write bits
//   base + 0x4  output      plain read-write bits
//   base + 0x8  toggle      write a one to flip a bit, writing zero is ignored
//   base + 0xC  input       read only
template<int Port>
constexpr unsigned PortBase = 0x100U + unsigned(Port) * 0x10U;

template<int Port>
using DirAddr = Kvasir::Register::Address<PortBase<Port>, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port>
using OutAddr
  = Kvasir::Register::Address<PortBase<Port> + 4, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port>
using ToggleAddr
  = Kvasir::Register::Address<PortBase<Port> + 8, 0xFFFFFFFF, 0x00000000, std::uint32_t>;

template<int Port>
using InAddr
  = Kvasir::Register::Address<PortBase<Port> + 12, 0x00000000, 0x00000000, std::uint32_t>;

template<int Port, int Pin>
using DirField = Kvasir::Register::FieldLocation<DirAddr<Port>,
                                                 (1U << unsigned(Pin)),
                                                 Kvasir::Register::ReadWriteAccess,
                                                 std::uint32_t>;

template<int Port, int Pin>
using OutField = Kvasir::Register::FieldLocation<OutAddr<Port>,
                                                 (1U << unsigned(Pin)),
                                                 Kvasir::Register::ReadWriteAccess,
                                                 std::uint32_t>;

using ToggleAccess
  = Kvasir::Register::Access<Kvasir::Register::AccessType::readWrite,
                             Kvasir::Register::ReadActionType::normal,
                             Kvasir::Register::ModifiedWriteValueType::oneToToggle>;

template<int Port, int Pin>
using ToggleField = Kvasir::Register::
  FieldLocation<ToggleAddr<Port>, (1U << unsigned(Pin)), ToggleAccess, std::uint32_t>;

template<int Port, int Pin>
using InField = Kvasir::Register::FieldLocation<InAddr<Port>,
                                                (1U << unsigned(Pin)),
                                                Kvasir::Register::ReadOnlyAccess,
                                                std::uint32_t>;

}   // namespace

// the chip file specialisations the pin factories resolve to
namespace Kvasir { namespace Io {

    template<PullConfiguration PC, int Port, int Pin>
    struct MakeAction<Action::Input<PC>, Register::PinLocation<Port, Pin>>
      : Register::Detail::Clear<DirField<Port, Pin>> {};

    template<OutputType OT, OutputSpeed OS, int Port, int Pin>
    struct MakeAction<Action::Output<OT, OS, OutputInit::Low>, Register::PinLocation<Port, Pin>>
      : Register::Detail::Set<DirField<Port, Pin>> {};

    template<OutputType OT, OutputSpeed OS, int Port, int Pin>
    struct MakeAction<Action::Output<OT, OS, OutputInit::High>, Register::PinLocation<Port, Pin>>
      : Register::Detail::Set<DirField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Set, Register::PinLocation<Port, Pin>>
      : Register::Detail::Set<OutField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Clear, Register::PinLocation<Port, Pin>>
      : Register::Detail::Clear<OutField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Toggle, Register::PinLocation<Port, Pin>>
      : Register::Detail::Set<ToggleField<Port, Pin>> {};

    template<int Port, int Pin>
    struct MakeAction<Action::Read, Register::PinLocation<Port, Pin>>
      : Register::Action<InField<Port, Pin>, Register::ReadAction> {};

}}   // namespace Kvasir::Io

using namespace Kvasir::Register;
using namespace Kvasir::Test;
using W = Recorder::Write;
using R = Recorder::Read;

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

// --- PinLocationEqual -----------------------------------------------------
static_assert(detail::PinLocationEqual(Loc<0,
                                           0>{},
                                       Loc<0,
                                           0>{}));
static_assert(!detail::PinLocationEqual(Loc<0,
                                            0>{},
                                        Loc<0,
                                            1>{}));
static_assert(!detail::PinLocationEqual(Loc<0,
                                            0>{},
                                        Loc<1,
                                            0>{}));
// anything that is not a pair of pin locations compares unequal
static_assert(!detail::PinLocationEqual(Loc<0,
                                            0>{},
                                        1));

// --- GetHwPort ------------------------------------------------------------
static_assert(detail::GetHwPort<Loc<0,
                                    5>>::value
              == 0);
static_assert(detail::GetHwPort<Loc<3,
                                    5>>::value
              == 3);

// --- OnSamePort -----------------------------------------------------------
static_assert(detail::OnSamePort<Loc<1,
                                     0>,
                                 Loc<1,
                                     7>>::value);
static_assert(!detail::OnSamePort<Loc<1,
                                      0>,
                                  Loc<2,
                                      0>>::value);
static_assert(!detail::OnSamePort<int,
                                  int>::value);

// --- GetPortNumbersT ------------------------------------------------------
// the pins are sorted and de-duplicated by port, and the port numbers extracted
static_assert(same<detail::GetPortNumbersT<brigand::list<Loc<0,
                                                             0>>>,
                   brigand::list<mpl::Int<0>>>);
static_assert(same<detail::GetPortNumbersT<brigand::list<Loc<0,
                                                             1>,
                                                         Loc<0,
                                                             0>>>,
                   brigand::list<mpl::Int<0>>>);
static_assert(same<detail::GetPortNumbersT<brigand::list<Loc<1,
                                                             0>,
                                                         Loc<0,
                                                             0>>>,
                   brigand::list<mpl::Int<0>,
                                 mpl::Int<1>>>);
static_assert(same<detail::GetPortNumbersT<brigand::list<Loc<2,
                                                             3>,
                                                         Loc<0,
                                                             1>,
                                                         Loc<2,
                                                             1>>>,
                   brigand::list<mpl::Int<0>,
                                 mpl::Int<2>>>);

// --- IsPort / IsSinglePort / IsDistributedPort ----------------------------
using SinglePort      = io::Port<io::PortAccess::defaultMode, Loc<0, 0>, Loc<0, 1>>;
using DistributedPort = io::Port<io::PortAccess::defaultMode, Loc<0, 0>, Loc<1, 1>>;

static_assert(detail::IsPort<SinglePort>::value);
static_assert(detail::IsPort<DistributedPort>::value);
static_assert(!detail::IsPort<Loc<0,
                                  0>>::value);
static_assert(!detail::IsPort<int>::value);

static_assert(detail::IsSinglePort<SinglePort>::value);
static_assert(!detail::IsSinglePort<DistributedPort>::value);
static_assert(!detail::IsSinglePort<int>::value);

static_assert(detail::IsDistributedPort<DistributedPort>::value);
static_assert(!detail::IsDistributedPort<SinglePort>::value);
static_assert(!detail::IsDistributedPort<int>::value);

// --- GetAccess ------------------------------------------------------------
static_assert(detail::GetAccess<SinglePort>::value == io::PortAccess::defaultMode);
static_assert(detail::GetAccess<io::Port<io::PortAccess::setClear,
                                         Loc<0,
                                             0>>>::value
              == io::PortAccess::setClear);
static_assert(detail::GetAccess<io::Port<io::PortAccess::toggle,
                                         Loc<0,
                                             0>>>::value
              == io::PortAccess::toggle);

// ===========================================================================
// makePinLocation / makePort
// ===========================================================================

static_assert(same<decltype(io::makePinLocation(io::HwPort<2>{},
                                                io::Pin<5>{})),
                   Loc<2,
                       5>>);
static_assert(same<decltype(io::makePinLocation(io::HwPort<0>{},
                                                io::Pin<0>{})),
                   Loc<0,
                       0>>);

// makePort from bare pin locations uses the default access mode
static_assert(same<decltype(io::makePort(Loc<0,
                                             0>{},
                                         Loc<0,
                                             1>{})),
                   io::Port<io::PortAccess::defaultMode,
                            Loc<0,
                                0>,
                            Loc<0,
                                1>>>);
// with an explicit access mode as the first argument
static_assert(same<decltype(io::makePort(io::Access::setClear,
                                         Loc<0,
                                             0>{},
                                         Loc<0,
                                             1>{})),
                   io::Port<io::PortAccess::setClear,
                            Loc<0,
                                0>,
                            Loc<0,
                                1>>>);

// ===========================================================================
// the pin factories, driven through apply()
// ===========================================================================

static void makeOutputSetsDirectionBit() {
    test("makeOutputSetsDirectionBit");

    apply(makeOutput(Loc<0, 3>{}));

    // the direction register has no write-ignored bits, so this is a read-modify-write
    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0>), 1U << 3);
}

static void makeInputClearsDirectionBit() {
    test("makeInputClearsDirectionBit");

    recorder.setReadValue(PortBase<0>, 0xFFFFFFFF);
    apply(makeInput(Loc<0, 3>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0>), 0xFFFFFFF7U);
}

static void makeOutputInitHigh() {
    test("makeOutputInitHigh");

    apply(makeOutputInitHigh(Loc<1, 2>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<1>), 1U << 2);
}

// the variadic form configures several pins at once; pins on the same port merge into a
// single register access
static void variadicMakeOutputSamePort() {
    test("variadicMakeOutputSamePort");

    apply(makeOutput(Loc<0, 0>{}, Loc<0, 1>{}, Loc<0, 2>{}));

    checkActionKinds("rw");
    CHECK_EQ(writeCount(PortBase<0>), 1U);
    CHECK_EQ(writtenValue(PortBase<0>), 0x7U);
}

static void variadicMakeOutputDifferentPorts() {
    test("variadicMakeOutputDifferentPorts");

    apply(makeOutput(Loc<0, 0>{}, Loc<1, 1>{}));

    // one read-modify-write per port
    CHECK_EQ(writeCount(PortBase<0>), 1U);
    CHECK_EQ(writeCount(PortBase<1>), 1U);
    CHECK_EQ(writtenValue(PortBase<0>), 0x1U);
    CHECK_EQ(writtenValue(PortBase<1>), 0x2U);
}

static void variadicMakeInput() {
    test("variadicMakeInput");

    recorder.setReadValue(PortBase<0>, 0xFFFFFFFF);
    apply(makeInput(Loc<0, 0>{}, Loc<0, 1>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0>), 0xFFFFFFFCU);
}

static void setPin() {
    test("setPin");

    apply(set(Loc<0, 5>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0> + 4), 1U << 5);
}

static void clearPin() {
    test("clearPin");

    recorder.setReadValue(PortBase<0> + 4, 0xFFFFFFFF);
    apply(clear(Loc<0, 5>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0> + 4), 0xFFFFFFDFU);
}

static void setAndClearDifferentPinsMerge() {
    test("setAndClearDifferentPinsMerge");

    recorder.setReadValue(PortBase<0> + 4, 0x00000000);
    apply(set(Loc<0, 0>{}), clear(Loc<0, 1>{}));

    // both pins live in the same output register, so one read-modify-write covers them
    CHECK_EQ(writeCount(PortBase<0> + 4), 1U);
    CHECK_EQ(writtenValue(PortBase<0> + 4), 0x1U);
}

static void togglePin() {
    test("togglePin");

    apply(toggle(Loc<0, 6>{}));

    // the toggle register is write-ignored-if-zero, so no read is needed
    checkActions({
      W{PortBase<0> + 8, 1U << 6}
    });
}

static void variadicToggle() {
    test("variadicToggle");

    apply(toggle(Loc<0, 0>{}, Loc<0, 1>{}, Loc<0, 2>{}));

    checkActions({
      W{PortBase<0> + 8, 0x7U}
    });
}

static void readPin() {
    test("readPin");

    recorder.setReadValue(PortBase<0> + 12, 1U << 4);
    auto const results = apply(read(Loc<0, 4>{}));

    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);
}

static void readPinLow() {
    test("readPinLow");

    recorder.setReadValue(PortBase<0> + 12, 0x00000000);
    auto const results = apply(read(Loc<0, 4>{}));

    CHECK_EQ(get<0>(results), 0U);
}

static void readTwoPinsSameRegister() {
    test("readTwoPinsSameRegister");

    recorder.setReadValue(PortBase<0> + 12, (1U << 2) | (1U << 7));
    auto const results = apply(read(Loc<0, 2>{}), read(Loc<0, 7>{}));

    // both pins are in the same input register, one read serves both
    checkActionKinds("r");
    CHECK_EQ(get<0>(results), 1U);
    CHECK_EQ(get<1>(results), 1U);
}

// the generic action() factory reaches the same chip specialisations
static void genericActionFactory() {
    test("genericActionFactory");

    apply(action(io::Action::set, Loc<0, 1>{}));

    checkActionKinds("rw");
    CHECK_EQ(writtenValue(PortBase<0> + 4), 1U << 1);
}

// configuring a pin and driving it in the same apply touches two different registers
static void configureAndDriveInOneApply() {
    test("configureAndDriveInOneApply");

    apply(makeOutput(Loc<0, 3>{}), set(Loc<0, 3>{}));

    CHECK_EQ(writeCount(PortBase<0>), 1U);       // direction
    CHECK_EQ(writeCount(PortBase<0> + 4), 1U);   // output
    CHECK_EQ(writtenValue(PortBase<0>), 1U << 3);
    CHECK_EQ(writtenValue(PortBase<0> + 4), 1U << 3);
}

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

    genericActionFactory();
    configureAndDriveInOneApply();

    return Kvasir::Test::report();
}
