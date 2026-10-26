// Kvasir::hexString (Util/HexString.hpp): two upper-case digits a byte, in the bytes' order - the text every chip's
// serialNumberString() returns for its serialNumber().
#include "kvasir/Util/HexString.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace {
template<std::size_t N>
std::string_view view(Kvasir::StaticString<N> const& s) {
    return std::string_view{s.data(), s.size()};
}
}   // namespace

int main() {
    using Kvasir::Test::test;
    {
        test("8 bytes (RP): 16 digits, upper case, the bytes' order");
        std::array const bytes{std::byte{0xE6},
                               std::byte{0x61},
                               std::byte{0x4C},
                               std::byte{0x31},
                               std::byte{0x1B},
                               std::byte{0x4E},
                               std::byte{0x5A},
                               std::byte{0x2C}};
        auto const       text = Kvasir::hexString(bytes);
        CHECK(view(text) == std::string_view{"E6614C311B4E5A2C"});
    }
    {
        test("12 bytes (STM32): 24 digits, zeros kept");
        std::array<std::byte, 12> bytes{};
        bytes[0]  = std::byte{0x01};
        bytes[11] = std::byte{0xF0};
        CHECK(view(Kvasir::hexString(bytes)) == std::string_view{"0100000000000000000000F0"});
        CHECK_EQ(Kvasir::hexString(bytes).size(), 24U);
    }
    {
        test("16 bytes (SAM): every digit value");
        std::array<std::byte, 16> bytes{};
        for(std::size_t i = 0; i != bytes.size(); ++i) {
            bytes[i] = std::byte{static_cast<unsigned char>(i * 0x11U)};
        }
        CHECK(view(Kvasir::hexString(bytes))
              == std::string_view{"00112233445566778899AABBCCDDEEFF"});
    }
    return Kvasir::Test::report();
}
