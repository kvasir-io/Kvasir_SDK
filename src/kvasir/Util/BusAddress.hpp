#pragma once
// The address of an object as a peripheral takes it: 32 bits in a register or a descriptor word - a DMA channel's
// read or write address, a vector table's base, a USB endpoint's buffer, a boot vector.
//
//     apply(write(Regs::BASEADDR::baseaddr, Kvasir::busAddress(descriptors.data())));
//     auto const at = Kvasir::busAddress(&entry) | 1U;   // a Thumb entry point
//
// busAddress(p) is the one place a pointer becomes that integer. On the chip a pointer has 32 bits and the value is
// the address. On a 64-bit host (a test on the register mock) it is the address's low 32 bits: no address there, but
// a header that asks for it compiles - reinterpret_cast<std::uint32_t>(pointer) is ill-formed where a pointer is
// wider, and a compiler says so as soon as it reads the line.
//
// BusAddress is for a parameter or a member that is either side of a transfer: a register's address (an integer,
// Regs::DATA::Addr::value) or memory (a pointer). The caller hands over what it has and writes no cast:
//
//     static void configure(Kvasir::BusAddress dest, Kvasir::BusAddress source, std::size_t count);
//     configure(buffer.data(), Regs::FIFO::Addr::value, buffer.size());
#include <cstdint>

namespace Kvasir {
// Objects and functions alike (T may be a function type).
template<typename T>
[[nodiscard,
  gnu::always_inline]] inline std::uint32_t
busAddress(T* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}

struct BusAddress {
    std::uint32_t value{};

    constexpr BusAddress() = default;

    // a register, or an address worked out already
    [[gnu::always_inline]] constexpr BusAddress(std::uint32_t address) : value{address} {}

    // memory
    template<typename T>
    [[gnu::always_inline]] BusAddress(T* p) : value{busAddress(p)} {}

    [[nodiscard]] friend constexpr bool operator==(BusAddress,
                                                   BusAddress) = default;
};
}   // namespace Kvasir
