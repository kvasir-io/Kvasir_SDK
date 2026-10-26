#pragma once

// Which pins a part has: one bit per pin, one mask per port, written next to the datasheet's
// pinout table in each chip package's `Io::PinLocationTraits`:
//
//     static constexpr int portBegin = 0;
//     static constexpr int portEnd   = 2;
//     static constexpr std::array<std::uint64_t, 2> bonded{0x0000'0000'DBFF'FFFFULL, ...};
//
// 64-bit masks: the RP2350B has 48 GPIOs on one port. Each family's
// `Io::isValidPinLocation<Port, Pin>()` asks `isBonded` with its traits.

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace Kvasir { namespace Hal { namespace Pins {
    template<typename T>
    concept BondedTraits = requires {
        { T::portBegin } -> std::convertible_to<int>;
        { T::portEnd } -> std::convertible_to<int>;
        { T::bonded[0] } -> std::convertible_to<std::uint64_t>;
    };

    /// Pins [begin, end) of one port as a mask: `range(0, 30)` = GPIO0..29.
    consteval std::uint64_t range(int begin,
                                  int end) {
        std::uint64_t m = 0;
        for(int p = begin; p < end; ++p) { m |= std::uint64_t{1} << p; }
        return m;
    }

    template<BondedTraits T>
    constexpr bool isBonded(int port,
                            int pin) {
        return port >= T::portBegin && port < T::portEnd && pin >= 0 && pin < 64
            && ((T::bonded[static_cast<std::size_t>(port - T::portBegin)] >> pin) & 1U) != 0;
    }
}}}   // namespace Kvasir::Hal::Pins
