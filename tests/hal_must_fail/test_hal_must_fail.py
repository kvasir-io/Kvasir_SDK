#!/usr/bin/env python3
"""kvasir/Hal/'s resources in Startup's checks: each case is a small image (ResourceCheck + Diagnose, what Startup's
ImageChecks instantiates) that must fail with a message naming what collided; the control must compile.

    test_hal_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = """#include "kvasir/Hal/Clocks.hpp"
#include "kvasir/Hal/Dma.hpp"
#include "kvasir/Hal/Owns.hpp"
#include "kvasir/Hal/Block.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/Resources.hpp"
namespace Chip {
template<unsigned I> struct ClkSercomCore : Kvasir::Hal::ClockTag {};
struct Sercom0 { static constexpr unsigned baseAddr = 0x42000800; };     // the I2C view of SERCOM0
struct Sercom0Spi { static constexpr unsigned baseAddr = 0x42000800; };  // the SPI view of the same block
struct Sercom1 { static constexpr unsigned baseAddr = 0x42000C00; };
struct Dmac {};
}
namespace Hal = Kvasir::Hal;
template<typename... R> struct ProvidesAll { using Provides = brigand::list<R...>; };
template<typename... R> struct ClaimsAll { using Claims = brigand::list<R...>; };
struct I2cMaster : ProvidesAll<Hal::Owns<Chip::Sercom0>> { using Claims = Hal::Claim<Chip::ClkSercomCore<0>, 48000000>; };
struct DmaInstance : ProvidesAll<Hal::DmaChannel<Chip::Dmac, 0>, Hal::DmaChannel<Chip::Dmac, 1>> {};
struct ClockSettings : ProvidesAll<Hal::Clk<Chip::ClkSercomCore<0>, 48000000>> {};
"""

ROWS = """namespace Gates {
using namespace Kvasir::Register;
inline constexpr FieldLocation<Address<0x40020000>, 1U << 3> resetUart{};
inline constexpr FieldLocation<Address<0x40020000>, 1U << 4> resetSpi{};
inline constexpr FieldLocation<Address<0x40030004>, 1U> uartCtrl{};
}
"""

GATES = ROWS + """template<> struct Kvasir::Hal::BlockGates<Kvasir::Tag::User> {
  using type = Kvasir::Hal::BlockTable<Kvasir::Hal::BlockRow<0x40030000, 0x4000, 3, decltype(clear(Gates::resetUart))>>; };
"""

TAIL = """using Check = Kvasir::Startup::ResourceCheck<brigand::list<{}>>;
static_assert(Kvasir::Startup::Diagnostics::Diagnose<Check>::value);
int main() {{ return 0; }}
"""

# (name, the image's own code, its Startup entries, the wanted message; None: must compile)
ROWS = [
    ("control: one owner per block, each clock provided (want None)",
     "struct SpiMaster : ProvidesAll<Hal::Owns<Chip::Sercom1>> {};\n",
     "ClockSettings, I2cMaster, SpiMaster", None),
    ("an I2C master and an SPI master on one SERCOM",
     "struct SpiMaster : ProvidesAll<Hal::Owns<Chip::Sercom0Spi>> {};\n",
     "ClockSettings, I2cMaster, SpiMaster",
     "a hardware resource is provided (configured, owned) by two peripherals: peripheral block at 0x42000800"),
    ("two drivers on one DMA channel",
     "struct SpiA : ClaimsAll<Hal::DmaChannel<Chip::Dmac, 0>> {};\nstruct SpiB : ClaimsAll<Hal::DmaChannel<Chip::Dmac, 0>> {};\n",
     "DmaInstance, SpiA, SpiB",
     "a hardware resource is claimed by two peripherals: give each its own: DMA channel 0"),
    ("a clock nothing provides (a SERCOM whose GCLK nobody routes)",
     "struct Bare {};\n",
     "Bare, I2cMaster",
     "a peripheral claims a hardware resource nothing in any Startup list provides: Chip::ClkSercomCore<0> 48000000"),
    ("control: a block written after its gate is opened (want None)",
     GATES +
     "struct Uart { static constexpr auto powerClockEnable = Kvasir::MPL::list(clear(Gates::resetUart));\n"
     "  static constexpr auto initStepPeripheryConfig = Kvasir::MPL::list(set(Gates::uartCtrl)); };\n",
     "Uart", None),
    ("a block written that no listed powerClockEnable opens",
     GATES +
     "struct Uart { static constexpr auto initStepPeripheryConfig = Kvasir::MPL::list(set(Gates::uartCtrl)); };\n",
     "Uart",
     "a peripheral writes a block that no listed powerClockEnable opens"),
    ("two block rows that overlap",
     ROWS +
     "using Table = Kvasir::Hal::BlockTable<Kvasir::Hal::BlockRow<0x40030000, 0x4000, 3, decltype(clear(Gates::resetUart))>,\n"
     "  Kvasir::Hal::BlockRow<0x40032000, 0x4000, 4, decltype(clear(Gates::resetSpi))>>;\n"
     "static_assert(Table::gateAt(0) == -1);\n",
     "", "block table: rows overlap"),
    ("a block row whose open action is not a literal field write",
     ROWS +
     "using Row = Kvasir::Hal::BlockRow<0x40030000, 0x4000, 3, decltype(write(Gates::resetUart, 0U))>;\n"
     "static_assert(Row::id == 3);\n",
     "", "a block row's open action is one literal field write"),
    ("a clock claimed at another rate than the tree provides",
     "struct I2cSlow { using Claims = Hal::Claim<Chip::ClkSercomCore<0>, 8000000>; };\n",
     "ClockSettings, I2cSlow",
     "nothing in any Startup list provides: Chip::ClkSercomCore<0> 8000000"),
]


CASES = [Case(name, extra + TAIL.format(entries), None if want is None else [want])
         for name, extra, entries, want in ROWS]

if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
