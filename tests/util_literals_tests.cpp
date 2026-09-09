// Tests for Kvasir::literals and the compile time constant parser behind it: number bases,
// digit separators, the type of every suffix and the range each suffix accepts.
//
// Everything the parser does happens at compile time, so this file is almost entirely
// static_asserts; main() only exists so the test can be run by ctest.
#include "kvasir/Util/literals.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace Kvasir::literals;

// ---------------------------------------------------------------------------
// every suffix produces the documented type
static_assert(std::is_same_v<decltype(1_i8),
                             std::int8_t>);
static_assert(std::is_same_v<decltype(1_i16),
                             std::int16_t>);
static_assert(std::is_same_v<decltype(1_i32),
                             std::int32_t>);
static_assert(std::is_same_v<decltype(1_i64),
                             std::int64_t>);
static_assert(std::is_same_v<decltype(1_u8),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(1_u16),
                             std::uint16_t>);
static_assert(std::is_same_v<decltype(1_u32),
                             std::uint32_t>);
static_assert(std::is_same_v<decltype(1_u64),
                             std::uint64_t>);
static_assert(std::is_same_v<decltype(1_b),
                             std::byte>);
static_assert(std::is_same_v<decltype(1_isize),
                             std::ptrdiff_t>);
static_assert(std::is_same_v<decltype(1_usize),
                             std::size_t>);
static_assert(std::is_same_v<decltype(1_i),
                             int>);
static_assert(std::is_same_v<decltype(1_u),
                             unsigned>);

// ---------------------------------------------------------------------------
// decimal
static_assert(0_u32 == 0);
static_assert(1_u32 == 1);
static_assert(9_u32 == 9);
static_assert(10_u32 == 10);
static_assert(42_u32 == 42);
static_assert(1234567890_u32 == 1234567890);

// ---------------------------------------------------------------------------
// hexadecimal, both prefix spellings and both digit cases
static_assert(0x0_u32 == 0);
static_assert(0x1_u32 == 1);
static_assert(0xff_u32 == 255);
static_assert(0xFF_u32 == 255);
static_assert(0Xff_u32 == 255);
static_assert(0XFF_u32 == 255);
static_assert(0xdeadbeef_u32 == 0xdeadbeefU);
static_assert(0xDEADBEEF_u32 == 0xdeadbeefU);
static_assert(0xAbCdEf_u32 == 0xabcdefU);
static_assert(0xffffffff_u32 == 0xffffffffU);
static_assert(0xffffffffffffffff_u64 == 0xffffffffffffffffULL);

// ---------------------------------------------------------------------------
// binary, both prefix spellings
static_assert(0b0_u32 == 0);
static_assert(0b1_u32 == 1);
static_assert(0b1010_u32 == 10);
static_assert(0B1010_u32 == 10);
static_assert(0b11111111_u32 == 255);
static_assert(0b10000000000000000000000000000000_u32 == 0x80000000U);

// ---------------------------------------------------------------------------
// octal: a leading 0 followed by more digits selects base 8
static_assert(00_u32 == 0);
static_assert(07_u32 == 7);
static_assert(010_u32 == 8);
static_assert(017_u32 == 15);
static_assert(0777_u32 == 511);
// a lone 0 is still zero and does not take the octal path
static_assert(0_u32 == 0);

// ---------------------------------------------------------------------------
// digit separators are ignored in every base and every position between digits
static_assert(1'000_u32 == 1000);
static_assert(1'0'0'0_u32 == 1000);
static_assert(0xde'ad'be'ef_u32 == 0xdeadbeefU);
static_assert(0b1010'1010_u32 == 0xaa);
static_assert(07'7'7_u32 == 511);
static_assert(1'2'3'4'5'6'7'8'9_u32 == 123456789);

// ---------------------------------------------------------------------------
// signed suffixes
static_assert(0_i8 == 0);
static_assert(127_i8 == 127);
static_assert(0x7f_i8 == 127);
static_assert(32767_i16 == 32767);
static_assert(2147483647_i32 == 2147483647);
static_assert(9223372036854775807_i64 == 9223372036854775807LL);
// note: the parser has no notion of a sign, `-1_i8` is unary minus applied to `1_i8`
static_assert(-1_i8 == -1);
static_assert(std::is_same_v<decltype(-1_i8),
                             int>);   // integral promotion

// ---------------------------------------------------------------------------
// the range accepted by each suffix is the range of its result type
static_assert(255_u8 == 255);
static_assert(0xff_u8 == 255);
static_assert(65535_u16 == 65535);
static_assert(4294967295_u32 == 4294967295U);
static_assert(18446744073709551615_u64 == 18446744073709551615ULL);
static_assert(255_b == std::byte{255});
static_assert(0xff_b == std::byte{0xff});
// _i and _u are validated against the minimum width the standard guarantees for int
static_assert(32767_i == 32767);
static_assert(65535_u == 65535);

// the following would each be a compile error ("integer literal overflow"), which is the
// property that makes these literals useful in register code:
//   256_u8   65536_u16   4294967296_u32   128_i8   65536_u   32768_i
// they cannot be expressed as a static_assert because the diagnostic is a hard error.

// ---------------------------------------------------------------------------
// the literals are usable in constant expressions in the places register code needs them
template<std::uint32_t V>
struct Tag {
    static constexpr std::uint32_t value = V;
};

static_assert(Tag<0xdead_u32>::value == 0xdead);
static_assert(std::array<std::byte,
                         3_usize>{}
                .size()
              == 3);

int main() {
    Kvasir::Test::test("literals");

    // a couple of runtime checks so the test binary does something observable and the
    // literals are exercised outside of a constant evaluation context too
    auto const hex = 0xcafe_u32;
    auto const bin = 0b1100'1010'1111'1110_u32;
    auto const dec = 51966_u32;
    auto const oct = 0145376_u32;

    CHECK_EQ(hex, 0xcafeU);
    CHECK_EQ(bin, 0xcafeU);
    CHECK_EQ(dec, 0xcafeU);
    CHECK_EQ(oct, 0xcafeU);

    CHECK_EQ(std::to_integer<unsigned>(0xa5_b), 0xa5U);

    return Kvasir::Test::report();
}
