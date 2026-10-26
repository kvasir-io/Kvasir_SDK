// Kvasir::Memory (Register/Memory.hpp): plain loads and stores of memory that is not a register, through
// Register::Mmio - every access reaches the mock with its address, in order, one per word.
#include "kvasir/Register/Memory.hpp"

#include "kvasir_test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace Kvasir::Test;

static_assert(std::is_same_v<decltype(Kvasir::Memory::load<std::uint8_t>(0)),
                             std::uint8_t>);
static_assert(std::is_same_v<decltype(Kvasir::Memory::load<std::uint16_t>(0)),
                             std::uint16_t>);
static_assert(std::is_same_v<decltype(Kvasir::Memory::load<std::uint32_t>(0)),
                             std::uint32_t>);

namespace {
void loadsEachWidth() {
    test("Memory::load: one read at the address, the value cut to the width");
    recorder.setReadValues(0x10, {0x1234'564DU});
    recorder.setReadValues(0x20, {0x1234'5678U});
    recorder.setReadValues(0x30, {0x1234'5678U});
    CHECK(Kvasir::Memory::load<std::uint8_t>(runtimeValue(0x10)) == 0x4DU);
    CHECK(Kvasir::Memory::load<std::uint16_t>(0x20) == 0x5678U);
    CHECK(Kvasir::Memory::load<std::uint32_t>(0x30) == 0x1234'5678U);
    checkActions({
      R{0x10, 0x1234'564DU},
      R{0x20, 0x1234'5678U},
      R{0x30, 0x1234'5678U}
    });
}

void storesEachWidth() {
    test("Memory::store: one write at the address, no read");
    Kvasir::Memory::store<std::uint8_t>(0x40, std::uint8_t{0xAB});
    Kvasir::Memory::store<std::uint16_t>(0x42, std::uint16_t{0xBEEF});
    Kvasir::Memory::store<std::uint32_t>(runtimeValue(0x44), 0xDEAD'BEEFU);
    checkActions({
      W{0x40,       0xABU},
      W{0x42,     0xBEEFU},
      W{0x44, 0xDEADBEEFU}
    });
}

void loadsWordsInOrder() {
    test("Memory::loadWords: one read per word, ascending addresses");
    recorder.setReadValues(0x100, {1U});
    recorder.setReadValues(0x104, {2U});
    recorder.setReadValues(0x108, {3U});
    std::array<std::uint32_t, 3> words{};
    Kvasir::Memory::loadWords(0x100, words.data(), words.size());
    CHECK(words == (std::array<std::uint32_t, 3>{1U, 2U, 3U}));
    checkActions({
      R{0x100, 1U},
      R{0x104, 2U},
      R{0x108, 3U}
    });

    test("Memory::loadWords: halfwords step by 2, a count of 0 reads nothing");
    std::array<std::uint16_t, 2> halves{};
    Kvasir::Memory::loadWords(0x200, halves.data(), halves.size());
    Kvasir::Memory::loadWords(0x300, halves.data(), 0);
    checkActions({
      R{0x200, 0U},
      R{0x202, 0U}
    });
}

void wordsByIndex() {
    test("Memory::Words: element i at base + i * sizeof(T), one access each, in the order called");
    recorder.setReadValues(0x408, {7U});
    Kvasir::Memory::Words<std::uint32_t> const page{runtimeValue(0x400)};
    page.store(0, 0x1111'1111U);
    page.store(3, 0x4444'4444U);
    CHECK(page.load(2) == 7U);
    Kvasir::Memory::Words<std::uint16_t> const halves{0x500};
    halves.store(1, std::uint16_t{0xBEEF});
    checkActions({
      W{0x400, 0x11111111U},
      W{0x40C, 0x44444444U},
      R{0x408,          7U},
      W{0x502,     0xBEEFU}
    });
}

void loadsBytesInOrder() {
    test(
      "Memory::loadBytes: the bytes in address order, out[i] from address + i; a count of 0 reads "
      "nothing");
    recorder.setReadValues(0x600, {0xA1U});
    recorder.setReadValues(0x601, {0xB2U});
    recorder.setReadValues(0x602, {0xC3U});
    std::array<std::byte, 3> bytes{};
    Kvasir::Memory::loadBytes(runtimeValue(0x600), bytes.data(), bytes.size());
    Kvasir::Memory::loadBytes(0x700, bytes.data(), 0);
    CHECK(bytes == (std::array<std::byte, 3>{std::byte{0xA1}, std::byte{0xB2}, std::byte{0xC3}}));
    checkActions({
      R{0x600, 0xA1U},
      R{0x601, 0xB2U},
      R{0x602, 0xC3U}
    });
}

void loadsPlainWords() {
    test("Memory::loadPlain: one load of a T at the address, as a word");
    recorder.setReadValues(0x800, {0x1234'5678U});
    CHECK_EQ(Kvasir::Memory::loadPlain<std::uint32_t>(runtimeValue(0x800)), 0x1234'5678U);
    checkActions({
      R{0x800, 0x1234'5678U}
    });
}
}   // namespace

int main() {
    loadsEachWidth();
    storesEachWidth();
    loadsWordsInOrder();
    wordsByIndex();
    loadsBytesInOrder();
    loadsPlainWords();

    return Kvasir::Test::report();
}
