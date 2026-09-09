// Tests for Kvasir::BitField: extracting a bit range out of a byte array and combining a
// value back into one.
//
// The interesting part of BitField is the shift/mask arithmetic for fields that do not
// start or end on a byte boundary, so the tests check it against an independent reference
// implementation over a wide sweep of (High, Low) pairs rather than against hand written
// expected values.
#include "kvasir/Util/BitField.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

template<std::size_t N>
using Bytes = std::array<std::byte, N>;

// ---------------------------------------------------------------------------
// reference implementation
//
// A byte array is treated as a little endian bit string: byte n holds bits
// [n*8 .. n*8+7], bit 0 of byte 0 being the least significant bit overall.

template<std::size_t N>
constexpr std::uint64_t toBits(Bytes<N> const& a) {
    std::uint64_t bits = 0;
    for(std::size_t n = 0; n != N; ++n) {
        bits |= static_cast<std::uint64_t>(std::to_integer<unsigned>(a[n])) << (8 * n);
    }
    return bits;
}

template<std::size_t N>
constexpr Bytes<N> fromBits(std::uint64_t bits) {
    Bytes<N> a{};
    for(std::size_t n = 0; n != N; ++n) { a[n] = std::byte((bits >> (8 * n)) & 0xFFU); }
    return a;
}

constexpr std::uint64_t widthMask(std::size_t width) {
    return width == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << width) - 1);
}

template<std::size_t High,
         std::size_t Low,
         std::size_t N>
constexpr std::uint64_t referenceExtract(Bytes<N> const& a) {
    return (toBits(a) >> Low) & widthMask(High - Low + 1);
}

template<std::size_t High,
         std::size_t Low,
         std::size_t N>
constexpr Bytes<N> referenceCombine(Bytes<N> const& a,
                                    std::uint64_t   value) {
    auto const fieldMask = widthMask(High - Low + 1) << Low;
    auto const bits      = (toBits(a) & ~fieldMask) | ((value << Low) & fieldMask);
    return fromBits<N>(bits);
}

// ---------------------------------------------------------------------------
// a handful of byte patterns to run every field against
template<std::size_t N>
constexpr std::array<Bytes<N>,
                     5>
patterns() {
    std::array<Bytes<N>, 5> ps{};
    for(std::size_t n = 0; n != N; ++n) {
        ps[0][n] = std::byte{0x00};
        ps[1][n] = std::byte{0xFF};
        ps[2][n] = std::byte{0xA5};
        ps[3][n] = std::byte(n * 37 + 1);
        ps[4][n] = std::byte(n % 2 == 0 ? 0x0F : 0xF0);
    }
    return ps;
}

// values to write into a field, trimmed to the field width
constexpr std::array<std::uint64_t, 7>
  rawValues{0, 1, 2, 0x55, 0xAA, 0x1234'5678, ~std::uint64_t{0}};

// ---------------------------------------------------------------------------
// one (High, Low, N) combination checked against the reference in both directions
template<std::size_t High,
         std::size_t Low,
         std::size_t N>
void checkField() {
    using Field = Kvasir::BitField<High, Low>;
    static_assert(N * 8 > High);

    for(auto const& p : patterns<N>()) {
        // extract
        auto const expected = referenceExtract<High, Low>(p);
        auto const got      = static_cast<std::uint64_t>(Field{p}.asValue());
        CHECK_EQ(got, expected);

        // isValid: with the default Min/Max every representable value is valid
        CHECK(Field::isValid(p));

        // combine
        for(auto raw : rawValues) {
            auto const value = raw & widthMask(High - Low + 1);

            auto array = p;
            Field{static_cast<typename Field::value_Type>(value)}.combineInto(array);

            auto const expectedArray = referenceCombine<High, Low>(p, value);
            CHECK(array == expectedArray);

            // round trip: what was written must read back
            CHECK_EQ(static_cast<std::uint64_t>(Field{array}.asValue()), value);
        }
    }
}

// sweep every Low for a given field width over an array big enough to hold it
template<std::size_t Width,
         std::size_t N,
         std::size_t... Lows>
void checkWidthImpl(std::index_sequence<Lows...>) {
    (checkField<Lows + Width - 1, Lows, N>(), ...);
}

template<std::size_t Width,
         std::size_t N,
         std::size_t LowCount>
void checkWidth() {
    checkWidthImpl<Width, N>(std::make_index_sequence<LowCount>{});
}

}   // namespace

// ---------------------------------------------------------------------------
// the value type is picked from the field width
static_assert(std::is_same_v<Kvasir::BitField<0,
                                              0>::value_Type,
                             std::uint8_t>);
static_assert(std::is_same_v<Kvasir::BitField<7,
                                              0>::value_Type,
                             std::uint8_t>);
static_assert(std::is_same_v<Kvasir::BitField<8,
                                              0>::value_Type,
                             std::uint16_t>);
static_assert(std::is_same_v<Kvasir::BitField<15,
                                              0>::value_Type,
                             std::uint16_t>);
static_assert(std::is_same_v<Kvasir::BitField<16,
                                              0>::value_Type,
                             std::uint32_t>);
static_assert(std::is_same_v<Kvasir::BitField<31,
                                              0>::value_Type,
                             std::uint32_t>);
// the width is what matters, not the absolute position
static_assert(std::is_same_v<Kvasir::BitField<20,
                                              13>::value_Type,
                             std::uint8_t>);
static_assert(std::is_same_v<Kvasir::BitField<20,
                                              12>::value_Type,
                             std::uint16_t>);

// ---------------------------------------------------------------------------
// a few fully worked out cases, so a failure points at a concrete number
static void handWorkedCases() {
    Kvasir::Test::test("handWorkedCases");

    // a nibble inside a single byte
    {
        using HighNibble = Kvasir::BitField<7, 4>;
        using LowNibble  = Kvasir::BitField<3, 0>;
        using Middle     = Kvasir::BitField<5, 2>;

        Bytes<1> const a{std::byte{0x5A}};
        CHECK_EQ(HighNibble{a}.asValue(), 0x5U);
        CHECK_EQ(LowNibble{a}.asValue(), 0xAU);
        CHECK_EQ(Middle{a}.asValue(), 0x6U);   // 0x5A >> 2 == 0b0110
    }

    // a field straddling one byte boundary
    {
        using Straddle = Kvasir::BitField<11, 4>;
        using Whole    = Kvasir::BitField<15, 0>;

        Bytes<2> const a{std::byte{0xF0}, std::byte{0x0F}};
        CHECK_EQ(Straddle{a}.asValue(), 0xFFU);
        CHECK_EQ(Whole{a}.asValue(), 0x0FF0U);
    }

    // a field straddling two byte boundaries: the middle byte has to be shifted too
    {
        using Wide = Kvasir::BitField<19, 4>;

        Bytes<3> const a{std::byte{0x30}, std::byte{0xAB}, std::byte{0x0C}};
        //  bits: 0x0C_AB_30 >> 4 == 0xCAB3, 16 bits wide
        CHECK_EQ(Wide{a}.asValue(), 0xCAB3U);

        // and writing it back must reproduce exactly those bytes
        Bytes<3> b{};
        Wide{0xCAB3U}.combineInto(b);
        CHECK_EQ(std::to_integer<unsigned>(b[0]), 0x30U);
        CHECK_EQ(std::to_integer<unsigned>(b[1]), 0xABU);
        CHECK_EQ(std::to_integer<unsigned>(b[2]), 0x0CU);
    }

    // a full 32 bit field that is not byte aligned
    {
        using Full32 = Kvasir::BitField<35, 4>;

        Bytes<5> a{};
        Full32{0xDEADBEEFU}.combineInto(a);
        CHECK_EQ(std::to_integer<unsigned>(a[0]), 0xF0U);
        CHECK_EQ(std::to_integer<unsigned>(a[1]), 0xEEU);
        CHECK_EQ(std::to_integer<unsigned>(a[2]), 0xDBU);
        CHECK_EQ(std::to_integer<unsigned>(a[3]), 0xEAU);
        CHECK_EQ(std::to_integer<unsigned>(a[4]), 0x0DU);
        CHECK_EQ(Full32{a}.asValue(), 0xDEADBEEFU);
    }
}

// combining must leave every bit outside the field alone
static void combinePreservesNeighbours() {
    Kvasir::Test::test("combinePreservesNeighbours");

    using Straddle = Kvasir::BitField<11, 4>;
    using Wide     = Kvasir::BitField<19, 4>;

    Bytes<3> const original{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};

    auto a = original;
    Straddle{0x00}.combineInto(a);
    CHECK_EQ(std::to_integer<unsigned>(a[0]), 0x0FU);
    CHECK_EQ(std::to_integer<unsigned>(a[1]), 0xF0U);
    CHECK_EQ(std::to_integer<unsigned>(a[2]), 0xFFU);

    auto b = original;
    Wide{0x0000}.combineInto(b);
    CHECK_EQ(std::to_integer<unsigned>(b[0]), 0x0FU);
    CHECK_EQ(std::to_integer<unsigned>(b[1]), 0x00U);
    CHECK_EQ(std::to_integer<unsigned>(b[2]), 0xF0U);
}

// Min/Max clamp on construction and drive isValid
static void minMaxClamping() {
    Kvasir::Test::test("minMaxClamping");

    using Clamped = Kvasir::BitField<7, 0, 10, 200>;

    CHECK_EQ(Clamped{5}.asValue(), 10U);
    CHECK_EQ(Clamped{10}.asValue(), 10U);
    CHECK_EQ(Clamped{100}.asValue(), 100U);
    CHECK_EQ(Clamped{200}.asValue(), 200U);
    CHECK_EQ(Clamped{255}.asValue(), 200U);

    // isValid reports whether the raw bits already satisfy the range
    CHECK(!Clamped::isValid(Bytes<1>{std::byte{5}}));
    CHECK(Clamped::isValid(Bytes<1>{std::byte{10}}));
    CHECK(Clamped::isValid(Bytes<1>{std::byte{200}}));
    CHECK(!Clamped::isValid(Bytes<1>{std::byte{201}}));

    // constructing from an array clamps as well
    CHECK_EQ(Clamped{Bytes<1>{std::byte{255}}}.asValue(), 200U);
    CHECK_EQ(Clamped{Bytes<1>{std::byte{0}}}.asValue(), 10U);
}

// the assignType parameter changes the type the field converts to and from
static void assignTypeConversion() {
    Kvasir::Test::test("assignTypeConversion");

    enum class Mode : std::uint8_t { off = 0, slow = 1, fast = 2 };

    using ModeField = Kvasir::BitField<3, 2, 0, 3, Mode>;

    Bytes<1> a{std::byte{0b0000'1000}};
    CHECK(static_cast<Mode>(ModeField{a}) == Mode::fast);

    Bytes<1> b{};
    ModeField{Mode::slow}.combineInto(b);
    CHECK_EQ(std::to_integer<unsigned>(b[0]), 0b0000'0100U);

    // plain fields convert to their value type implicitly
    using Plain          = Kvasir::BitField<7, 0>;
    std::uint8_t const v = Plain{0x42};
    CHECK_EQ(v, 0x42U);
}

static void equality() {
    Kvasir::Test::test("equality");

    using Field = Kvasir::BitField<7, 4>;
    CHECK(Field{3} == Field{3});
    CHECK(!(Field{3} == Field{4}));

    // values are sanitized before comparison
    using Clamped = Kvasir::BitField<7, 0, 0, 100>;
    CHECK(Clamped{200} == Clamped{150});   // both clamp to 100
}

// the whole thing is constexpr
static_assert([] {
    Bytes<3> a{};
    Kvasir::BitField<19, 4>{0xCAB3U}.combineInto(a);
    return Kvasir::BitField<19, 4>{a}.asValue() == 0xCAB3U && a[1] == std::byte{0xAB};
}());

int main() {
    handWorkedCases();
    combinePreservesNeighbours();
    minMaxClamping();
    assignTypeConversion();
    equality();

    // exhaustive sweeps: every field width against every start offset that fits
    Kvasir::Test::test("sweep width 1");
    checkWidth<1, 3, 24>();
    Kvasir::Test::test("sweep width 2");
    checkWidth<2, 3, 23>();
    Kvasir::Test::test("sweep width 3");
    checkWidth<3, 3, 22>();
    Kvasir::Test::test("sweep width 4");
    checkWidth<4, 3, 21>();
    Kvasir::Test::test("sweep width 5");
    checkWidth<5, 3, 20>();
    Kvasir::Test::test("sweep width 7");
    checkWidth<7, 3, 18>();
    Kvasir::Test::test("sweep width 8");
    checkWidth<8, 3, 17>();
    Kvasir::Test::test("sweep width 9");
    checkWidth<9, 3, 16>();
    Kvasir::Test::test("sweep width 12");
    checkWidth<12, 4, 21>();
    Kvasir::Test::test("sweep width 16");
    checkWidth<16, 4, 17>();
    Kvasir::Test::test("sweep width 17");
    checkWidth<17, 5, 24>();
    Kvasir::Test::test("sweep width 24");
    checkWidth<24, 5, 17>();
    Kvasir::Test::test("sweep width 32");
    checkWidth<32, 6, 17>();

    return Kvasir::Test::report();
}
