// Tests for kvasir/Hal/Block.hpp: a chip's block-gate table, its lookups, and the provides and claims Startup derives
// from it. A TU of its own: the table is the image's BlockGates<Tag::User>, which every Startup list here sees.
//
// Compile time only, like hal_tests.cpp: if this file compiles, the tests passed. The error TEXTS are hal_must_fail's.
#include "kvasir/Hal/Block.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <type_traits>

namespace Fake {
using namespace Kvasir::Register;
// A reset register (a gate opens by clearing its bit) and a clock mask register (by setting it).
using ResetAddr = Address<0x40020000>;
using MaskAddr  = Address<0x40020004>;
inline constexpr FieldLocation<ResetAddr, 1U << 3> resetUart{};
inline constexpr FieldLocation<ResetAddr, 1U << 4> resetSpi{};
inline constexpr FieldLocation<MaskAddr, 1U << 6>  ahbUsb{};
inline constexpr FieldLocation<MaskAddr, 1U << 5>  apbUsb{};
inline constexpr FieldLocation<MaskAddr, 1U << 1>  apbEic{};

// Registers inside the blocks.
inline constexpr FieldLocation<Address<0x40030000>, 0x1> uartData{};   // UART at 0x40030000, 16 KiB
inline constexpr FieldLocation<Address<0x40034010>, 0x1> spiCtrl{};    // SPI at 0x40034000
inline constexpr FieldLocation<Address<0x41005000>, 0x1> usbCtrl{};    // USB: two gates
inline constexpr FieldLocation<Address<0x40001800>, 0x1> eicCtrl{};    // EIC: open at boot
inline constexpr FieldLocation<Address<0x50000000>, 0x1> sioOut{};     // in no block

using UartRow = Kvasir::Hal::BlockRow<0x40030000, 0x4000, 3, decltype(clear(resetUart))>;
using SpiRow  = Kvasir::Hal::BlockRow<0x40034000, 0x4000, 4, decltype(clear(resetSpi))>;
using UsbAhb  = Kvasir::Hal::BlockRow<0x41005000, 0x400, 38, decltype(set(ahbUsb))>;
using UsbApb  = Kvasir::Hal::BlockRow<0x41005000, 0x400, 37, decltype(set(apbUsb))>;
using EicRow  = Kvasir::Hal::BlockRow<0x40001800, 0x400, 33, decltype(set(apbEic)), true>;
using Table   = Kvasir::Hal::BlockTable<UartRow, SpiRow, UsbAhb, UsbApb, EicRow>;

struct PinTag {
    static constexpr bool sharedClaim = true;
    static constexpr bool coreLocal   = false;
};
}   // namespace Fake

template<>
struct Kvasir::Hal::BlockGates<Kvasir::Tag::User> {
    using type = Fake::Table;
};

namespace {
namespace Hal = Kvasir::Hal;
using namespace Fake;
using Kvasir::Startup::ClaimOfAction;
using Kvasir::Startup::ResourceCheck;
using Kvasir::Startup::ResourceOfAction;

template<typename... Ts>
using tlist = brigand::list<Ts...>;

// ---- the table's lookups ---------------------------------------------------------------
static_assert(Table::gateAt(0x40030000) == 3 && Table::gateAt(0x40033FFC) == 3);
static_assert(Table::gateAt(0x40034000) == 4);
static_assert(Table::gateAt(0x40038000) == -1 && Table::gateAt(0x50000000) == -1);
static_assert(Table::gateAt(0x40001800) == 33);   // open at boot: still a block
static_assert(Table::claimsAt(0x40030004) == 1 && Table::claimsAt(0x41005000) == 2);
static_assert(Table::claimsAt(0x40001800) == 0);   // open at boot claims nothing
static_assert(Table::rowsOf(0x41005000) == 2 && Table::rowsOf(0x40030000) == 1
              && Table::rowsOf(0x1000) == 0);

// opens<>: by register, mask and value, not by spelling
static_assert(Table::opens<decltype(clear(resetUart))>() == 3);
static_assert(Table::opens<decltype(write(resetUart,
                                          value<unsigned,
                                                0>()))>()
              == 3);
static_assert(Table::opens<decltype(set(resetUart))>() == -1);   // puts it INTO reset
static_assert(Table::opens<decltype(set(apbUsb))>() == 37);
static_assert(Table::opens<decltype(set(uartData))>() == -1);
static_assert(Table::opens<int>() == -1);

// OpenActions: one action, a list for two gates, an empty list for no block
static_assert(std::is_same_v<Table::OpenActions<0x40030000>,
                             decltype(clear(resetUart))>);
static_assert(std::is_same_v<Table::OpenActions<0x41005000>,
                             tlist<decltype(set(ahbUsb)),
                                   decltype(set(apbUsb))>>);
static_assert(std::is_same_v<Table::OpenActions<0x1000>,
                             tlist<>>);
static_assert(std::is_same_v<Table::ClaimsAt<0x41005000>,
                             tlist<Hal::Gate<38>,
                                   Hal::Gate<37>>>);

// ---- derived provides and claims, through the image's hook ----------------------------
static_assert(std::is_same_v<ResourceOfAction<decltype(clear(resetUart))>::type,
                             tlist<Hal::Gate<3>>>);
static_assert(std::is_same_v<ResourceOfAction<decltype(set(resetUart))>::type,
                             tlist<>>);
static_assert(std::is_same_v<ClaimOfAction<decltype(set(uartData))>::type,
                             tlist<Hal::Gate<3>>>);
static_assert(std::is_same_v<ClaimOfAction<decltype(read(spiCtrl))>::type,
                             tlist<Hal::Gate<4>>>);
static_assert(std::is_same_v<ClaimOfAction<decltype(set(usbCtrl))>::type,
                             tlist<Hal::Gate<38>,
                                   Hal::Gate<37>>>);
static_assert(std::is_same_v<ClaimOfAction<decltype(set(eicCtrl))>::type,
                             tlist<>>);
static_assert(std::is_same_v<ClaimOfAction<decltype(set(sioOut))>::type,
                             tlist<>>);

struct Uart {
    static constexpr auto powerClockEnable        = list(clear(resetUart));
    static constexpr auto initStepPeripheryConfig = list(set(uartData));
};

struct UartNoOpener {
    static constexpr auto initStepPeripheryConfig = list(set(uartData));
};

struct
  UartSecond {   // a second driver on the same block opens it too: one opening (mergeIdentical)
    static constexpr auto powerClockEnable        = list(clear(resetUart));
    static constexpr auto initStepPeripheryEnable = list(set(uartData));
};

struct Usb {
    static constexpr auto powerClockEnable        = list(set(ahbUsb), set(apbUsb));
    static constexpr auto initStepPeripheryConfig = list(set(usbCtrl));
};

struct UsbOneGate {
    static constexpr auto powerClockEnable        = list(set(ahbUsb));
    static constexpr auto initStepPeripheryConfig = list(set(usbCtrl));
};

struct Eic {   // open at boot: no opener needed
    static constexpr auto initStepPeripheryConfig = list(set(eicCtrl));
};

static_assert(ResourceCheck<tlist<Uart>>::value);
static_assert(ResourceCheck<tlist<Uart,
                                  UartSecond>>::value);
static_assert(!ResourceCheck<tlist<UartNoOpener>>::claimsProvided);
static_assert(ResourceCheck<tlist<Uart>,
                            tlist<UartNoOpener>>::value);   // the gate is not core-local
static_assert(ResourceCheck<tlist<Usb>>::value);
static_assert(!ResourceCheck<tlist<UsbOneGate>>::claimsProvided);
static_assert(ResourceCheck<tlist<Eic>>::value);

// ---- beside a chip's own ResourceOfAction (a pin's function select, RP Io.hpp's shape) --
inline constexpr Kvasir::Register::FieldLocation<Kvasir::Register::Address<0x40014004>, 0x1F>
  gpio0Funcsel{};
}   // namespace

namespace Kvasir { namespace Startup {
    template<unsigned Z,
             unsigned O,
             typename RT,
             typename Mode,
             unsigned Mask,
             typename Acc,
             typename FT,
             unsigned V>
    struct ResourceOfAction<Register::Action<
      Register::FieldLocation<Register::Address<0x40014004, Z, O, RT, Mode>, Mask, Acc, FT>,
      Register::WriteLiteralAction<V>>> {
        using type = brigand::list<Resource<Fake::PinTag, 0>>;
    };
}}   // namespace Kvasir::Startup

namespace {
// disjoint: the pin write provides the pin, the reset release the gate, no ambiguity
static_assert(std::is_same_v<ResourceOfAction<decltype(write(gpio0Funcsel,
                                                             value<unsigned,
                                                                   5>()))>::type,
                             tlist<Kvasir::Startup::Resource<PinTag,
                                                             0>>>);
static_assert(std::is_same_v<ResourceOfAction<decltype(clear(resetSpi))>::type,
                             tlist<Hal::Gate<4>>>);
}   // namespace

int main() { return Kvasir::Test::report(); }
