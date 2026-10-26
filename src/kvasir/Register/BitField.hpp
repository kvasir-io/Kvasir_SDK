#pragma once
// Kvasir::BitField<High, Low, Min, Max, Assign>: the bit range High..Low of a little-endian byte image (bit 0 = bit 0
// of byte 0), such as an NVM row. The value is clamped to Min..Max when it is set; it converts to and from Assign (an
// enum of the field's values, or the value type). At most 32 bits wide; a field is read and written through a uint64
// window over the bytes it touches (32 bits at bit offset 7 span 5 bytes).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir {

namespace Detail {
    template<std::size_t Size, typename = void>
    struct GetBitFieldValueType {
        using type = std::uint32_t;
    };

    template<std::size_t Size>
    struct GetBitFieldValueType<Size, std::enable_if_t<(Size <= 7)>> {
        using type = std::uint8_t;
    };

    template<std::size_t Size>
    struct GetBitFieldValueType<Size, std::enable_if_t<(Size > 7 && Size <= 15)>> {
        using type = std::uint16_t;
    };

    // Size is High - Low, so this also catches High < Low (it underflows to a huge Size). It is the first thing the
    // compiler evaluates for a BitField (the default Min/Max need the value type), so its message comes first.
    template<std::size_t Size>
    struct GetBitFieldValueType<Size, std::enable_if_t<(Size > 31)>> {
        static_assert(Size <= 31,
                      "BitField<High, Low>: High >= Low and at most 32 bits wide");
        using type = std::uint32_t;
    };

    template<std::size_t Size>
    using GetIndexTypeT = typename GetBitFieldValueType<Size, void>::type;

    // the low sizeMinusOne + 1 bits (sizeMinusOne <= 31: no overflow)
    constexpr std::uint64_t bitFieldMask(std::size_t sizeMinusOne) {
        return (std::uint64_t{1} << (sizeMinusOne + 1)) - 1;
    }
}   // namespace Detail

template<std::size_t High, std::size_t Low>
using BitFieldValueType = Detail::GetIndexTypeT<High - Low>;

template<std::size_t                  High,
         std::size_t                  Low,
         BitFieldValueType<High, Low> Min = 0,
         BitFieldValueType<High, Low> Max
         = static_cast<BitFieldValueType<High, Low>>(Detail::bitFieldMask(High - Low)),
         typename Assign = BitFieldValueType<High, Low>>
struct BitField {
    static_assert(31 >= High - Low,
                  "BitField<High, Low>: at most 32 bits wide");
    static_assert(High >= Low);
    static_assert(Max >= Min);
    static_assert(Detail::bitFieldMask(High - Low) >= Max);

    using value_type = BitFieldValueType<High, Low>;

    constexpr BitField(Assign a) : v{sanitize(static_cast<value_type>(a))} {}

    constexpr operator Assign() const { return static_cast<Assign>(v); }

    [[nodiscard]] constexpr value_type asValue() const { return v; }

    template<std::size_t N>
    constexpr explicit BitField(std::array<std::byte,
                                           N> const& a)
      : v{sanitize(extract(a))} {}

    // true when the image holds a value inside Min..Max
    template<std::size_t N>
    static constexpr bool isValid(std::array<std::byte,
                                             N> const& a) {
        auto const raw = extract(a);
        return raw == sanitize(raw);
    }

    // writes the field's bits of the image, leaves every other bit as it is
    template<std::size_t N>
    constexpr void combineInto(std::array<std::byte,
                                          N>& a) const {
        auto const w = (window(a) & ~(mask << shift)) | (std::uint64_t{v} << shift);
        for(std::size_t i = first; i <= last; ++i) {
            a[i] = std::byte(static_cast<unsigned char>(w >> (8 * (i - first))));
        }
    }

    constexpr bool operator==(BitField const&) const = default;

private:
    static constexpr std::size_t   first = Low / 8;
    static constexpr std::size_t   last  = High / 8;
    static constexpr std::size_t   shift = Low % 8;
    static constexpr std::uint64_t mask  = Detail::bitFieldMask(High - Low);
    static_assert(last - first < 8,
                  "the window is one uint64");

    template<std::size_t N>
    static constexpr std::uint64_t window(std::array<std::byte,
                                                     N> const& a) {
        static_assert(N * 8 > High, "BitField: the image is too short for High");
        std::uint64_t w = 0;
        for(std::size_t i = first; i <= last; ++i) {
            w |= std::uint64_t{std::to_integer<unsigned char>(a[i])} << (8 * (i - first));
        }
        return w;
    }

    template<std::size_t N>
    static constexpr value_type extract(std::array<std::byte,
                                                   N> const& a) {
        return static_cast<value_type>((window(a) >> shift) & mask);
    }

    static constexpr value_type sanitize(value_type x) {
        return std::clamp<value_type>(x, Min, Max);
    }

    value_type v;
};

}   // namespace Kvasir
