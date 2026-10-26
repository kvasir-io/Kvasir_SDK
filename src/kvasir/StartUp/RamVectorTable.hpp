#pragma once
#include "core/Barrier.hpp"
#include "core_peripherals/SCB.hpp"
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Port/Interrupts.hpp"
#include "kvasir/Register/Memory.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// The boot core's vector table in RAM.
//
// On a flash build every exception entry fetches its vector from flash, which fails while
// the flash cannot be read (after a clk_sys resus, Clocks::Resus, the QSPI timing no longer
// fits the clock). A RAM table also removes the flash latency from every interrupt entry.
//
// A Startup-list peripheral: copies the table VTOR points at into an aligned RAM array and
// points VTOR at the copy, after the interrupt config steps and before the NVIC is enabled.
// The copy is identical to the original, so nothing observes the switch. Provides
// `RamVectorTableTag`, which Resus claims.
//
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, Clock, ...,
//                                              Kvasir::Startup::RamVectorTable<>, Resus>;
//
// Entries: the table's size in words, a power of two at or above the chip's count (16 core
// exceptions + IRQs: 16 + 52 = 68 on the RP2350, RP2350 datasheet 3.2 "52 interrupt inputs"
// (RP2350_Datasheet.md l.3746); 16 + 32 = 48 on the RP2040, whose NVIC has 32 inputs with
// IRQs 26-31 tied to zero, RP2040 datasheet 2.3.2 (RP2040_Datasheet.md l.2923-2925) - the
// generated interruptEnd is 52 / 32); it is also the alignment VTOR requires. A size below the chip's count fails the build: the IRQs past it would fetch zero
// vectors. Reading past the real table copies whatever follows it, harmlessly. Traits is the
// chip's interrupt table (chip/Interrupt.hpp), a parameter only so that a host test can
// name another (core_cortex_common tests/decoders, over the generated SCB).
namespace Kvasir { namespace Startup {
    struct RamVectorTableTag {
        static constexpr bool sharedClaim = true;
    };

    template<std::size_t Entries = 128,
             typename Traits     = Nvic::InterruptOffsetTraits<Kvasir::Tag::User>>
    struct RamVectorTable {
        static_assert(Entries >= 32 && (Entries & (Entries - 1)) == 0,
                      "Entries is the table size in words and its alignment: a power of two");
        static_assert(
          Entries >= 16 + static_cast<std::size_t>(Traits::end),
          "Entries is below this chip's vector count (16 core exceptions + "
          "InterruptOffsetTraits::end IRQs): the IRQs past it would fetch zero vectors");

        using Provides = brigand::list<Resource<RamVectorTableTag, 0>>;

        alignas(Entries * sizeof(std::uint32_t)) inline static std::array<std::uint32_t,
                                                                          Entries> table{};

        using Vtor = Kvasir::Peripheral::SCB::Registers<>::VTOR;

        static void preEnableRuntimeInit() {
            // the table VTOR points at now (flash, or a RAM image's own): memory, read as such
            Kvasir::Memory::loadWords(Kvasir::Register::readWhole(Vtor::FULLREGISTER),
                                      table.data(),
                                      Entries);
            Kvasir::Core::dsb();
            Kvasir::Register::writeWhole(Vtor::FULLREGISTER,
                                         reinterpret_cast<std::uint32_t>(table.data()));
            Kvasir::Core::dsb();
            Kvasir::Core::isb();
        }

        /// Whether VTOR points at the RAM copy.
        [[nodiscard]] static bool active() {
            return Kvasir::Register::readWhole(Vtor::FULLREGISTER)
                == reinterpret_cast<std::uint32_t>(table.data());
        }
    };
}}   // namespace Kvasir::Startup
