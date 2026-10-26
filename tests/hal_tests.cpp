// Tests for kvasir/Hal/: the vocabulary the chip families share (clocks, block owners, DMA
// channels, UART streams, bonded pins) and Startup::ResourceIds underneath it.
//
// Compile time only, like resources_tests.cpp: if this file compiles, the tests passed, and every
// rule is checked both ways. The error TEXTS are hal_must_fail's.
#include "kvasir/Hal/Clocks.hpp"
#include "kvasir/Hal/Dma.hpp"
#include "kvasir/Hal/Owns.hpp"
#include "kvasir/Hal/Pins.hpp"
#include "kvasir/Hal/Spi.hpp"
#include "kvasir/Hal/Uart.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

namespace {

namespace Hal = Kvasir::Hal;
using Kvasir::Startup::Resource;
using Kvasir::Startup::ResourceCheck;

template<typename... Ts>
using list = brigand::list<Ts...>;

template<typename... Rs>
struct Provider {
    using Provides = list<Rs...>;
};

template<typename... Rs>
struct Claimer {
    using Claims = list<Rs...>;
};

// ---- Startup::ResourceIds / isResourceOf -----------------------------------------------
struct SomeTag {};

struct OtherTag {};

using R3 = Resource<SomeTag, 3, 7U>;
static_assert(std::is_same_v<Kvasir::Startup::ResourceIds<R3>::tag,
                             SomeTag>);
static_assert(Kvasir::Startup::ResourceIds<R3>::ids.size() == 2);
static_assert(Kvasir::Startup::ResourceIds<R3>::ids[0] == 3
              && Kvasir::Startup::ResourceIds<R3>::ids[1] == 7);
static_assert(Kvasir::Startup::isResourceOf<R3,
                                            SomeTag>);
static_assert(!Kvasir::Startup::isResourceOf<R3,
                                             OtherTag>);
static_assert(!Kvasir::Startup::isResourceOf<int,
                                             SomeTag>);

// ---- Hal/Clocks.hpp --------------------------------------------------------------------
struct PeriClk : Hal::ClockTag {};

struct SercomClk : Hal::ClockTag {};

struct OtherClk : Hal::ClockTag {};

static_assert(PeriClk::sharedClaim && !PeriClk::coreLocal);
template<typename T>
constexpr bool statesOptionalProvider = requires { T::optionalProvider; };
static_assert(!statesOptionalProvider<PeriClk>);   // every clock claim needs a provider
static_assert(std::is_same_v<Hal::Claim<SercomClk,
                                        48'000'000>,
                             list<Hal::Clk<SercomClk,
                                           48'000'000>>>);

using Tree = list<Hal::Clk<PeriClk, 150'000'000>,
                  Hal::Clk<SercomClk, 48'000'000>,
                  Resource<SomeTag, 1>>;   // not a clock: ignored
static_assert(Hal::providersOf<PeriClk>(Tree{}) == 1);
static_assert(Hal::providersOf<OtherClk>(Tree{}) == 0);
static_assert(Hal::providersOf<SercomClk>(list<Hal::Clk<SercomClk,
                                                        1>,
                                               Hal::Clk<SercomClk,
                                                        2>>{})
              == 2);
static_assert(Hal::hzOf<PeriClk>(Tree{}) == 150'000'000);
static_assert(Hal::hzOf<SercomClk,
                        Tree>()
              == 48'000'000);

using Clocks  = Provider<Hal::Clk<PeriClk, 150'000'000>, Hal::Clk<SercomClk, 48'000'000>>;
using Uart150 = Claimer<Hal::Clk<PeriClk, 150'000'000>>;
using Uart200 = Claimer<Hal::Clk<PeriClk, 200'000'000>>;

struct Sercom48 {
    using Claims = Hal::Claim<SercomClk, 48'000'000>;
};

using Sercom8   = Claimer<Hal::Clk<SercomClk, 8'000'000>>;
using SercomOth = Claimer<Hal::Clk<OtherClk, 48'000'000>>;

// a claim at the rate the tree provides passes, a claim at another rate is refused
static_assert(ResourceCheck<list<Clocks,
                                 Uart150,
                                 Sercom48>>::value);
static_assert(!ResourceCheck<list<Clocks,
                                  Uart200>>::claimsProvided);
static_assert(!ResourceCheck<list<Clocks,
                                  Sercom8>>::claimsProvided);
// nothing provides the tag at all (a ClockSettings without Provides): refused all the same
static_assert(!ResourceCheck<list<Uart200>>::claimsProvided);
static_assert(!ResourceCheck<list<Sercom48>>::claimsProvided);
static_assert(!ResourceCheck<list<Clocks,
                                  SercomOth>>::claimsProvided);
// several drivers on one clock are fine (sharedClaim), on either core (not coreLocal)
static_assert(ResourceCheck<list<Clocks,
                                 Sercom48,
                                 Sercom48>>::noDoubleClaim);
static_assert(ResourceCheck<list<Clocks>,
                            list<Sercom48>>::value);

// ---- Hal/Owns.hpp ----------------------------------------------------------------------
struct RegsA {
    static constexpr std::uint32_t baseAddr = 0x4200'0800;
};

struct RegsAasSpi {   // another view of the same block (a SERCOM in SPI mode)
    static constexpr std::uint32_t baseAddr = 0x4200'0800;
};

struct RegsB {
    static constexpr std::uint32_t baseAddr = 0x4200'0C00;
};

static_assert(std::is_same_v<Hal::Owns<RegsA>,
                             Hal::OwnsAt<0x4200'0800>>);
static_assert(std::is_same_v<Hal::Owns<RegsA>,
                             Hal::Owns<RegsAasSpi>>);
static_assert(ResourceCheck<list<Provider<Hal::Owns<RegsA>>,
                                 Provider<Hal::Owns<RegsB>>>>::value);
static_assert(!ResourceCheck<list<Provider<Hal::Owns<RegsA>>,
                                  Provider<Hal::Owns<RegsAasSpi>>>>::noDoubleProvide);
// across the cores too: one block, one owner in the whole image
static_assert(!ResourceCheck<list<Provider<Hal::Owns<RegsA>>>,
                             list<Provider<Hal::Owns<RegsA>>>>::noDoubleProvide);

// ---- Hal/Dma.hpp -----------------------------------------------------------------------
struct DmaA {};

struct DmaB {};

using DmaInstA = Provider<Hal::DmaChannel<DmaA, 0>, Hal::DmaChannel<DmaA, 1>>;
using DmaInstB = Provider<Hal::DmaChannel<DmaB, 0>>;
using SpiOnA0  = Claimer<Hal::DmaChannel<DmaA, 0>>;
using UartOnA0 = Claimer<Hal::DmaChannel<DmaA, 0U>>;   // the id's integer type is not part of it
using UartOnB0 = Claimer<Hal::DmaChannel<DmaB, 0>>;

static_assert(ResourceCheck<list<DmaInstA,
                                 DmaInstB,
                                 SpiOnA0,
                                 UartOnB0>>::value);   // two controllers' ch0
static_assert(!ResourceCheck<list<DmaInstA,
                                  SpiOnA0,
                                  UartOnA0>>::noDoubleClaim);   // one controller's ch0
static_assert(!ResourceCheck<list<SpiOnA0>>::claimsProvided);   // instance in no list
static_assert(!ResourceCheck<list<DmaInstA>,
                             list<SpiOnA0>>::claimsLocal);   // the other core's
static_assert(!ResourceCheck<list<DmaInstA,
                                  DmaInstA>>::noDoubleProvide);

// ---- Hal/Spi.hpp -----------------------------------------------------------------------
static_assert(std::is_same_v<std::underlying_type_t<Hal::ClockMode>,
                             std::uint8_t>);
static_assert(static_cast<int>(Hal::ClockMode::mode3) == 3);

// ---- Hal/Uart.hpp ----------------------------------------------------------------------
struct GoodUart {
    static bool receive(std::optional<std::byte>&) { return false; }

    static std::size_t send(std::span<std::byte const> s) { return s.size(); }

    static bool txIdle() { return true; }
};

struct ConvenienceOnly {   // `std::optional<std::byte> receive()`: not the shared shape

    static std::optional<std::byte> receive() { return std::nullopt; }
};

struct SendVoid {   // a send that does not say how much it took

    static void send(std::span<std::byte const>) {}

    static bool txIdle() { return true; }
};

struct NoIdle {
    static std::size_t send(std::span<std::byte const> s) { return s.size(); }
};

static_assert(Hal::Uart::ByteSource<GoodUart> && Hal::Uart::ByteSink<GoodUart>);
static_assert(!Hal::Uart::ByteSource<ConvenienceOnly>);
static_assert(!Hal::Uart::ByteSink<SendVoid>);
static_assert(!Hal::Uart::ByteSink<NoIdle>);
static_assert(static_cast<int>(Hal::Uart::DataBits::bits9) == 4
              && static_cast<int>(Hal::Uart::Parity::even) == 2);

// ---- Hal/Pins.hpp ----------------------------------------------------------------------
struct OnePort48 {   // an RP2350B: 48 GPIOs on one port
    static constexpr int                          portBegin = 0;
    static constexpr int                          portEnd   = 1;
    static constexpr std::array<std::uint64_t, 1> bonded{Hal::Pins::range(0, 48)};
};

struct TwoPorts {   // a SAM-like part: PA without 26 and 29, PB 2..3 and 22..23
    static constexpr int                          portBegin = 0;
    static constexpr int                          portEnd   = 2;
    static constexpr std::array<std::uint64_t, 2> bonded{0xDBFF'FFFFULL, 0x00C0'000CULL};
};

struct OffsetPorts {   // ports 1..2 (portBegin != 0)
    static constexpr int                          portBegin = 1;
    static constexpr int                          portEnd   = 3;
    static constexpr std::array<std::uint64_t, 2> bonded{0x1ULL, 0x8000'0000'0000'0000ULL};
};

static_assert(Hal::Pins::BondedTraits<OnePort48> && !Hal::Pins::BondedTraits<GoodUart>);
static_assert(Hal::Pins::range(0,
                               0)
                == 0
              && Hal::Pins::range(0,
                                  30)
                   == 0x3FFF'FFFFULL);
static_assert(Hal::Pins::range(0,
                               64)
              == ~0ULL);
static_assert(Hal::Pins::isBonded<OnePort48>(0,
                                             31)
              && Hal::Pins::isBonded<OnePort48>(0,
                                                32)
              && Hal::Pins::isBonded<OnePort48>(0,
                                                47));
static_assert(!Hal::Pins::isBonded<OnePort48>(0,
                                              48)
              && !Hal::Pins::isBonded<OnePort48>(0,
                                                 63)
              && !Hal::Pins::isBonded<OnePort48>(0,
                                                 64)
              && !Hal::Pins::isBonded<OnePort48>(0,
                                                 -1));
static_assert(!Hal::Pins::isBonded<OnePort48>(1,
                                              0)
              && !Hal::Pins::isBonded<OnePort48>(-1,
                                                 0));
static_assert(Hal::Pins::isBonded<TwoPorts>(0,
                                            25)
              && !Hal::Pins::isBonded<TwoPorts>(0,
                                                26)
              && !Hal::Pins::isBonded<TwoPorts>(0,
                                                29)
              && Hal::Pins::isBonded<TwoPorts>(0,
                                               31));
static_assert(!Hal::Pins::isBonded<TwoPorts>(1,
                                             0)
              && Hal::Pins::isBonded<TwoPorts>(1,
                                               2)
              && Hal::Pins::isBonded<TwoPorts>(1,
                                               23)
              && !Hal::Pins::isBonded<TwoPorts>(1,
                                                32));
static_assert(!Hal::Pins::isBonded<TwoPorts>(2,
                                             0));
static_assert(!Hal::Pins::isBonded<OffsetPorts>(0,
                                                0)
              && Hal::Pins::isBonded<OffsetPorts>(1,
                                                  0)
              && Hal::Pins::isBonded<OffsetPorts>(2,
                                                  63)
              && !Hal::Pins::isBonded<OffsetPorts>(3,
                                                   63));

}   // namespace

int main() { return Kvasir::Test::report(); }
