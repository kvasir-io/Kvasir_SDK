#pragma once

// Bytes as upper-case hex text, two digits a byte in the bytes' order: the chips' serialNumberString() (a USB serial
// string, a log line) from their serialNumber().
//
//     auto const text = Kvasir::hexString(Kvasir::serialNumber());   // "E6614C311B4E5A2C" for 8 bytes
#include "StaticString.hpp"

#include <array>
#include <cstddef>

namespace Kvasir {
template<std::size_t N>
StaticString<2 * N> hexString(std::array<std::byte,
                                         N> const& bytes) {
    StaticString<2 * N> text;
    static constexpr std::array<char, 16>
      hexDigits{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

    for(auto const& byte : bytes) {
        text.push_back(hexDigits[(static_cast<int>(byte) & 0xF0) >> 4]);
        text.push_back(hexDigits[static_cast<int>(byte) & 0x0F]);
    }

    return text;
}
}   // namespace Kvasir
