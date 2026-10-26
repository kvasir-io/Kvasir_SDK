#pragma once
// Memory that is not a register - flash and XIP windows, OTP and NVM rows, ROM tables, calibration words - read and
// written as plain words of a width, through the register layer's one access (Register::Mmio): always inlined (a
// RAM function with XIP off may use it) and visible to the host mock (Test::readAt / writeAt). Registers go through
// the field API (apply) or Register::RegisterTraits, not through this.
//
//     auto const magic = Kvasir::Memory::load<std::uint8_t>(0x10);        // one byte load
//     std::uint32_t words[3];
//     Kvasir::Memory::loadWords(0x10, words, 3);                           // three word loads, in address order
//     Kvasir::Memory::Words<std::uint32_t> const page{0x2000};             // a page, element by element:
//     page.store(i, v);                                                    // one store of word i
//     std::array<std::byte, 16> rec;
//     Kvasir::Memory::loadBytes(0x10001000, rec.data(), rec.size());       // a byte copy (memcpy)
//     auto const id = Kvasir::Memory::loadPlain<std::uint32_t>(0x0080A00C); // a word that never changes (not volatile)
#include "Mmio.hpp"

#include <cstddef>
#include <cstdint>

namespace Kvasir { namespace Memory {
    // one load of a T at the address
    template<typename T>
    [[gnu::always_inline]] inline T load(std::uintptr_t address) {
        return Register::Mmio<T>::load(address);
    }

    // one store of a T at the address
    template<typename T>
    [[gnu::always_inline]] inline void store(std::uintptr_t address,
                                             T              v) {
        Register::Mmio<T>::store(address, v);
    }

    // a run of T from a base address, element by element: `Words<std::uint32_t> const page{address};` before the
    // loop, `page.store(i, v)` / `page.load(i)` in it. A loop over words takes this form, not load(base + 4 * i): the
    // pointer is made once and indexed, as a loop over a volatile pointer was (Register::MmioWords)
    template<typename T>
    using Words = Register::MmioWords<T>;

    // `count` words of a T from the address on, one load each in ascending order (not memcpy: that may read bytewise
    // or out of order; not a std::span: its operator[] is no always_inline call, and a RAM function would call flash)
    template<typename T>
    [[gnu::always_inline]] inline void loadWords(std::uintptr_t address,
                                                 T*             out,
                                                 std::size_t    count) {
        Words<T> const from{address};
        for(std::size_t i = 0; i != count; ++i) { out[i] = from.load(i); }
    }

    // `count` bytes from the address on, one std::memcpy (it may read in any width and order: memory, not a register).
    // Not in a RAM function - memcpy is a call into flash (check_ram_funcs refuses it): loadWords there. Pointer and
    // count like loadWords (a span's members are no always_inline calls).
    [[gnu::always_inline]] inline void loadBytes(std::uintptr_t address,
                                                 std::byte*     out,
                                                 std::size_t    count) {
        Register::mmioLoadBytes(address, out, count);
    }

    // one aligned load of a T from memory that does not change while the program runs (a serial number, calibration
    // rows): not volatile, so the compiler may merge, move or drop it like any read of a constant - where load() is a
    // volatile access in program order. A word the hardware or the program changes (flash being written, a DMA
    // target) is load()'s.
    template<typename T>
    [[gnu::always_inline]] inline T loadPlain(std::uintptr_t address) {
        return Register::mmioLoadPlain<T>(address);
    }
}}   // namespace Kvasir::Memory
