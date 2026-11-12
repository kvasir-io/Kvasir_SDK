// busAddress / BusAddress (Util/BusAddress.hpp) on a host, where a pointer is wider than the 32 bits a peripheral
// takes: the header compiles, the value is the address's low 32 bits for objects and functions alike, and a
// BusAddress parameter takes a register's address and a pointer without a cast at the call.
#include "kvasir/Util/BusAddress.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <type_traits>

using Kvasir::BusAddress;
using Kvasir::busAddress;

namespace {
// an integer stays a constant: a register's address in a constexpr descriptor
static_assert(BusAddress{0x4000'0010U}.value == 0x4000'0010U);
static_assert(BusAddress{}.value == 0);
static_assert(BusAddress{7U} == BusAddress{7U} && !(BusAddress{7U} == BusAddress{8U}));
static_assert(std::is_convertible_v<std::uint32_t,
                                    BusAddress>);
static_assert(std::is_convertible_v<int*,
                                    BusAddress>);
static_assert(std::is_convertible_v<std::byte const*,
                                    BusAddress>);
static_assert(std::is_convertible_v<void volatile*,
                                    BusAddress>);
static_assert(std::is_convertible_v<void (*)(),
                                    BusAddress>);
// no way back to a pointer, and no arithmetic on it by accident
static_assert(!std::is_convertible_v<BusAddress,
                                     std::uint32_t>);
static_assert(std::is_same_v<decltype(busAddress(static_cast<int*>(nullptr))),
                             std::uint32_t>);

std::uint32_t low32(std::uintptr_t v) { return static_cast<std::uint32_t>(v & 0xFFFF'FFFFU); }

void function() {}

std::uint32_t taken(BusAddress a) { return a.value; }

void lowBitsOfAnObject() {
    std::array<std::uint16_t, 8> buffer{};
    auto const                   expected = low32(reinterpret_cast<std::uintptr_t>(buffer.data()));
    CHECK(busAddress(buffer.data()) == expected);
    CHECK(busAddress(static_cast<void const*>(buffer.data())) == expected);
    // element steps are the pointer's, as at a call site that names the block's end
    CHECK(busAddress(buffer.data() + buffer.size()) == low32(expected + 16U));
}

void lowBitsOfAFunction() {
    CHECK(busAddress(&function) == low32(reinterpret_cast<std::uintptr_t>(&function)));
    CHECK((busAddress(&function) | 1U)
          == (low32(reinterpret_cast<std::uintptr_t>(&function)) | 1U));
}

void parameterTakesBoth() {
    std::array<std::byte, 4> buffer{};
    CHECK(taken(0xd000'0000U) == 0xd000'0000U);
    CHECK(taken(buffer.data()) == busAddress(buffer.data()));
    CHECK(taken(&function) == busAddress(&function));
    CHECK(taken(0) == 0);   // a literal zero is the integer, not a null pointer
}
}   // namespace

int main() {
    lowBitsOfAnObject();
    lowBitsOfAFunction();
    parameterTakesBoth();
    return Kvasir::Test::report();
}
