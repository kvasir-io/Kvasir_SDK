#pragma once
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// VTOR through the core package's SCB (every firmware has it; a host test of the list rules does not, and uses only
// the table's size)
#if __has_include("core_peripherals/SCB.hpp")
    #include "core_peripherals/SCB.hpp"
    #include "kvasir/Register/Register.hpp"
    #define KVASIR_RAM_VECTOR_TABLE_SCB 1
#else
    #define KVASIR_RAM_VECTOR_TABLE_SCB 0
#endif

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
// exceptions + IRQs: 68 on the RP2350, 42 on the RP2040); it is also the alignment VTOR
// requires. A size below the chip's count fails the build: the IRQs past it would fetch zero
// vectors. Reading past the real table copies whatever follows it, harmlessly. Traits is the
// chip's interrupt table (chip/Interrupt.hpp), a parameter only so that a host test can
// name another.
namespace Kvasir { namespace Startup {
    struct RamVectorTableTag {
        static constexpr bool sharedClaim = true;
    };

    template<std::size_t Entries = 128, typename Traits = Nvic::InterruptOffsetTraits<>>
    struct RamVectorTable {
        static_assert(Entries >= 32 && (Entries & (Entries - 1)) == 0,
                      "Entries is the table size in words and its alignment: a power of two");
        static_assert(
          Entries >= static_cast<std::size_t>(16 + Traits::end),
          "Entries is below this chip's vector count (16 core exceptions + "
          "InterruptOffsetTraits::end IRQs): the IRQs past it would fetch zero vectors");

        using Provides = brigand::list<Resource<RamVectorTableTag, 0>>;

        alignas(Entries * sizeof(std::uint32_t)) inline static std::array<std::uint32_t,
                                                                          Entries> table{};

#if KVASIR_RAM_VECTOR_TABLE_SCB
        using Vtor = Kvasir::Peripheral::SCB::Registers<>::VTOR;

        static void preEnableRuntimeInit() {
            // the table VTOR points at now (flash, or a RAM image's own): memory, read as such
            auto const* src = reinterpret_cast<std::uint32_t const volatile*>(
              Kvasir::Register::readWhole(Vtor::FULLREGISTER));
            for(std::size_t i = 0; i < Entries; ++i) { table[i] = src[i]; }
            asm volatile("dsb" ::: "memory");
            Kvasir::Register::writeWhole(Vtor::FULLREGISTER,
                                         reinterpret_cast<std::uint32_t>(table.data()));
            asm volatile("dsb\n isb" ::: "memory");
        }

        /// Whether VTOR points at the RAM copy.
        [[nodiscard]] static bool active() {
            return Kvasir::Register::readWhole(Vtor::FULLREGISTER)
                == reinterpret_cast<std::uint32_t>(table.data());
        }
#endif
    };
}}   // namespace Kvasir::Startup
