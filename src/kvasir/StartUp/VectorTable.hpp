#pragma once

// The vector table builder of both cores: one constexpr fill of a std::array, O(vectors + ISRs). Host-compilable:
// the interrupt traits are a parameter (core 0 and core 1 pass the chip's Nvic::InterruptOffsetTraits<Tag::User>,
// a host test its own), and nothing here needs a chip or a core package.

#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/Port/Interrupts.hpp"

#include <array>
#include <cstddef>

namespace Kvasir { namespace Startup { namespace Detail {

    // The table VTOR points at: word 0 the initial SP, word 1 the reset entry, then one slot per vector index
    // Traits::begin .. Traits::end - 1 (slot 2 + index - begin), each the ISR a list entry installs there or
    // Nvic::UnusedIsr (disabled indexes too). IsrList holds anything with a `value` (the function) and an `IType`
    // (its Nvic::Index): Nvic::Isr, a generated dispatcher, an ISR wrapper. The list rules (each index once, every
    // index inside the table) are checked before the table is built (Checks.hpp); an index outside it is still a
    // compile error here (not a constant expression), a duplicate one would be "last one wins".
    template<Nvic::IsrFunctionPointer StackEnd,
             Nvic::IsrFunctionPointer Reset,
             typename Traits,
             typename IsrList>
    struct VectorTable;

    template<Nvic::IsrFunctionPointer StackEnd,
             Nvic::IsrFunctionPointer Reset,
             typename Traits,
             typename... Is>
    struct VectorTable<StackEnd, Reset, Traits, brigand::list<Is...>> {
        static constexpr std::size_t entries
          = 2 + static_cast<std::size_t>(Traits::end - Traits::begin);

        static consteval std::array<Nvic::IsrFunctionPointer,
                                    entries>
        fill() {
            std::array<Nvic::IsrFunctionPointer, entries> t{};
            t.fill(Nvic::UnusedIsr::value);
            t[0] = StackEnd;
            t[1] = Reset;
            ((t[2 + static_cast<std::size_t>(Is::IType::value - Traits::begin)] = Is::value), ...);
            return t;
        }

        std::array<Nvic::IsrFunctionPointer, entries> data = fill();
    };

    // The alignment VTOR demands of a table: the next power of two at or above its size, at least 128 bytes. Used for
    // a SecondaryCore's table, which lives at an arbitrary address; the boot core's sits at the start of the image
    // and is aligned by the linker script.
    constexpr std::size_t vectorTableAlignment(std::size_t entries) {
        std::size_t alignment = 128;
        while(alignment < entries * sizeof(Nvic::IsrFunctionPointer)) { alignment *= 2; }
        return alignment;
    }

}}}   // namespace Kvasir::Startup::Detail
