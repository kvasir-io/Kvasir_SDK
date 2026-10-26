#pragma once

// Block ownership as a Startup resource (kvasir/StartUp/Resources.hpp): a driver that
// configures a peripheral block provides `Hal::Owns<Regs>`, and two drivers on one block are
// Startup's rule 2 ("provided by two peripherals") with the block's base address in the error:
//
//     a hardware resource is provided (configured, owned) by two peripherals:
//       peripheral block at 0x42001400 - Kvasir::Sercom::I2C::...
//
// The key is the register set's base address, not the driver kind, so an I2C master and a
// USART on one SAM SERCOM (one base address, three register views) collide as they should,
// and one driver kind on two instances does not.
//
//     using Provides = brigand::list<Kvasir::Hal::Owns<Regs>>;
//
// Left without an owner on purpose (blocks several drivers share by design): a DMA controller
// (drivers share it by channel: Hal/Dma.hpp), a PIO block (by state machine), timers that
// drivers share by channel.

#include "kvasir/StartUp/Resources.hpp"

#include <cstdint>

namespace Kvasir { namespace Hal {
    struct OwnerTag {
        static constexpr bool        sharedClaim = false;
        static constexpr bool        coreLocal   = false;
        static constexpr char const* describeAs  = "peripheral block at";
        static constexpr bool        describeHex = true;   // "... at 0x42001400"
    };

    template<std::uint32_t Base>
    using OwnsAt = Startup::Resource<OwnerTag, Base>;

    /// `Regs` is a generated peripheral's `Registers<I>` (its `baseAddr`).
    template<typename Regs>
    using Owns = OwnsAt<Regs::baseAddr>;
}}   // namespace Kvasir::Hal
