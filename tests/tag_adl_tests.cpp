// The tags Nvic::Index, Io::HwPort and Io::Pin derive from std::integral_constant, so namespace std is an associated
// namespace of every call with a tag argument (or an argument whose type is templated on a tag). This file spells
// the unqualified idioms the firmwares use - no using-directive, no qualification, argument-dependent lookup only -
// so a std candidate that hard-errors (std::apply, std::get, std::size ...) shows up here. CI builds it against
// libstdc++ and libc++.
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Io/Io.hpp"
#include "kvasir/Io/PinFactories.hpp"
#include "kvasir/Io/Types.hpp"
#include "kvasir_test.hpp"

#include <cstdint>
#include <type_traits>

namespace {
// one enable register for the interrupt lines (a bit per line), one GPIO port (output at 0x204, input at 0x20C)
using EnableAddr = Kvasir::Register::Address<0x1E0, 0xFFFFFFFF, 0x00000000, std::uint32_t>;

template<int I>
using EnableBit = Kvasir::Register::FieldLocation<EnableAddr,
                                                  (1U << static_cast<unsigned>(I)),
                                                  Kvasir::Register::WriteOnlyAccess,
                                                  bool>;

template<int Pin>
using OutField
  = Kvasir::Register::FieldLocation<Kvasir::Register::Address<0x204, 0, 0, std::uint32_t>,
                                    (1U << static_cast<unsigned>(Pin)),
                                    Kvasir::Register::ReadWriteAccess,
                                    std::uint32_t>;

template<int Pin>
using InField
  = Kvasir::Register::FieldLocation<Kvasir::Register::Address<0x20C, 0, 0, std::uint32_t>,
                                    (1U << static_cast<unsigned>(Pin)),
                                    Kvasir::Register::ReadOnlyAccess,
                                    std::uint32_t>;
}   // namespace

namespace Kvasir::Nvic {
template<int I>
struct MakeAction<Action::Enable, Index<I>>
  : decltype(MPL::list(Register::write(EnableBit<I>{}, Register::value<bool, true>()))){};
}   // namespace Kvasir::Nvic

namespace Kvasir::Io {
template<int Pin>
struct MakeAction<Action::Set, PinLocation<0, Pin>> : Register::Detail::Set<OutField<Pin>> {};

template<int Pin>
struct MakeAction<Action::Read, PinLocation<0, Pin>>
  : Register::Action<InField<Pin>, Register::ReadAction> {};
}   // namespace Kvasir::Io

namespace {
constexpr auto led    = makePinLocation(Kvasir::Io::port0, Kvasir::Io::pin3);
constexpr auto button = makePinLocation(Kvasir::Io::port0, Kvasir::Io::Pin<5>{});

// the tags are integral constants that do not pass as a runtime int
static_assert(Kvasir::MPL::IntegralConstant<Kvasir::Nvic::Index<3>>);
static_assert(!std::is_convertible_v<Kvasir::Nvic::Index<3>,
                                     int>);
static_assert(!std::is_convertible_v<decltype(Kvasir::Io::pin3),
                                     int>);

void enableOneLine() {
    Kvasir::Test::test("enableOneLine");
    apply(makeEnable(Kvasir::Nvic::Index<3>{}));
    CHECK_EQ(Kvasir::Test::writtenValue(0x1E0), 1U << 3);
}

void enableAListOfLines() {
    Kvasir::Test::test("enableAListOfLines");
    // the argument is a brigand::list of tags: std is associated through each element's base
    apply(makeEnable(brigand::list<Kvasir::Nvic::Index<3>, Kvasir::Nvic::Index<4>>{}));
    CHECK_EQ(Kvasir::Test::writtenValue(0x1E0), (1U << 3) | (1U << 4));
}

void setAndReadPins() {
    Kvasir::Test::test("setAndReadPins");
    apply(set(led));
    CHECK_EQ(Kvasir::Test::writtenValue(0x204), 1U << 3);

    Kvasir::Test::recorder.setReadValue(0x20C, 1U << 5);
    auto const r = apply(read(button));
    CHECK_EQ(get<0>(r), 1U);   // the field's value, bit 5 of the input register
}
}   // namespace

int main() {
    enableOneLine();
    enableAListOfLines();
    setAndReadPins();
    return Kvasir::Test::report();
}
