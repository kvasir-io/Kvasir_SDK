# kvasir/Hal: the vocabulary between the chips and everything above them

`kvasir/Hal/` is not a driver layer. It holds the words every chip family uses the same way - resource tags the
Startup checker (`kvasir/StartUp/Resources.hpp`) counts, concepts a driver satisfies, small enums of a bus - so a
driver, a library above the chips or a firmware can name a clock, a block, a DMA channel or a flash region without
knowing the family. Everything lives in `namespace Kvasir::Hal` (but for `Block.hpp`'s two `Kvasir::Startup` traits,
`ResourceOfAction` / `ClaimOfAction`, through which the Startup checker sees a block's gates); the headers include only the standard library and
the port layer's own headers (`kvasir/Mpl`, `kvasir/Common/Tags.hpp`, `kvasir/Port`, `kvasir/StartUp/Resources.hpp`,
`kvasir/Register/Types.hpp` for the gate actions; `tools/layers.toml`).

| header | vocabulary | what a chip hands in | what it buys |
|---|---|---|---|
| `Clocks.hpp` | `Hal::Clk<Which, Hz>`, `Hal::Claim<Which, Hz>`, `hzOf<Which>(Provides)`, `ClockTag` | its clock tags (`Clocks::ClkPeri`, `ClkSercomCore<n>`, ...: chip data, derived from `ClockTag`); its ClockSettings `Provides` each clock at the rate it programs | a driver claims the clock its block counts at its config's rate; a rate nothing provides does not build |
| `Owns.hpp` | `Hal::Owns<Regs>`, `Hal::OwnsAt<Base>` | the generated `Registers<I>::baseAddr` | two drivers on one peripheral block are a build error naming the base address |
| `Block.hpp` | `Hal::BlockGates<Tag::User>` (a hook), `Hal::BlockTable<Hal::BlockRow<...>...>` | one table: each gated block's address range, its gate id and the action that opens it | a Startup entry whose actions write into a block no listed entry opens is a build error (run-time `apply()` is not checked) |
| `Dma.hpp` | `Hal::DmaChannel<Controller, Ch>` | its DMA instance `Provides` its channels | two drivers on one channel, a channel of an instance in no list or in the other core's list: build errors |
| `DmaHandoff.hpp` | `Hal::handToHardware()`, `Hal::takeFromHardware()` | nothing (the barrier per core architecture is in the header, with its citations) | descriptors and buffers another bus master reads are plain memory with two marked hand-over points |
| `Flash.hpp` | `Hal::NorRegion` (concept), `Hal::FlashGeometry`, `Hal::PageSplit`, `Hal::FlashRegionBase<Derived, Geometry, Split>` | a region that derives from `FlashRegionBase` and supplies `readAt`, `programPage`, `eraseSector` (and may hide `outsideImage()`) | the bounds, the page split (or refusal across a page; an empty program is true), the image check, written once; `Flash::CellStore` takes any `NorRegion` |
| `Pins.hpp` | `Hal::Pins::BondedTraits`, `range`, `isBonded` | per part one `bonded` mask per port, next to the datasheet's pinout | a pin the package does not have is refused where the pin is named |
| `Spi.hpp` | `Hal::ClockMode` | its drivers map it to the block's CPOL/CPHA | one spelling of the SPI mode for every family and the queued SPI engine |
| `Uart.hpp` | `Hal::Uart::{DataBits, Parity, StopBits}`, concepts `ByteSource`, `ByteSink` | its UART drivers take the frame format and offer `receive` / `send` / `txIdle` | code that only moves bytes (a parser, a console) takes any family's UART |

## Rules

- A new word goes here when two chip families need it the same way and a layer above the chips names it; a word
  only one family has stays in that chip package.
- No registers and no chip headers: a chip's data (its clock tags, its gate table, its bonded masks) comes in as
  template arguments or through a `<Tag::User>` hook (`Port/Hooks.hpp`), so every Hal header compiles before any
  chip header and in the host tests.
- A resource tag says how it is shared (`sharedClaim`: every user claims, e.g. a clock; or one owner, e.g. a block)
  and whether it belongs to one core (`coreLocal`). The checks are Startup's; the texts of their errors name the tag
  and the number (`describeAs`, `describeHex`).
- Host tests: `tests/flash_region_tests.cpp` (FlashRegionBase), `hal_block_tests.cpp` (the gate table),
  `resources_tests.cpp` (the resource checks), and the chips' conformance trees (each chip's region is a
  `Hal::NorRegion`).
