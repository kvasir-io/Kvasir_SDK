// Kvasir::Hal::FlashRegionBase (Hal/Flash.hpp): a fake Derived that records what it is asked - the bounds, a program split
// into whole 0xFF-padded pages or refused across a page (an empty one true under both), erase past the end, and outsideImage() against an image end
// the fake supplies (imageEnd() hidden: no linker symbol on the host). Then a CellStore over such a region: refused
// over the image, working past it.
#include "kvasir/Hal/Flash.hpp"

#include "kvasir/Runtime/CellStore.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace flash_region_test {
constexpr std::uint32_t base = 0x1000'8000;

struct Call {
    char          what;   // 'p' program, 'e' erase, 'r' read
    std::uint32_t at;
};

template<Kvasir::Hal::PageSplit Split>
struct Fake
  : Kvasir::Hal::FlashRegionBase<Fake<Split>, Kvasir::Hal::FlashGeometry{base, 64, 16, 4}, Split> {
    static constexpr std::uint32_t           pageBytes = 16;
    static inline std::array<std::byte, 256> mem       = [] {
        std::array<std::byte, 256> m{};
        m.fill(std::byte{0xFF});
        return m;
    }();
    static inline std::vector<Call> calls{};
    static inline std::uint32_t     end{base};   // the image's end

    static void readAt(std::uint32_t        a,
                       std::span<std::byte> out) {
        calls.push_back({'r', a});
        std::memcpy(out.data(), mem.data() + a, out.size());
    }

    static bool programPage(std::uint32_t        page,
                            std::span<std::byte const,
                                      pageBytes> bytes) {
        calls.push_back({'p', page});
        for(std::size_t i = 0; i != pageBytes; ++i) {
            mem[page + i] &= bytes[i];
        }   // NOR: bits only cleared
        return true;
    }

    static bool eraseSector(std::uint32_t s) {
        calls.push_back({'e', s});
        std::fill_n(mem.begin() + (std::size_t{s} * 64), 64, std::byte{0xFF});
        return true;
    }

    static std::uint32_t imageEnd() { return end; }

    static void reset() {
        calls.clear();
        mem.fill(std::byte{0xFF});
        end = base;
    }
};

using Split  = Fake<Kvasir::Hal::PageSplit::split>;
using Refuse = Fake<Kvasir::Hal::PageSplit::refuse>;

static_assert(Kvasir::Hal::NorRegion<Split> && Kvasir::Hal::NorRegion<Refuse>);
static_assert(Split::sectorBytes == 64 && Split::pageBytes == 16 && Split::sectorCount() == 4
              && Split::bytes == 256);

std::array<std::byte,
           24>
bytes(std::byte v) {
    std::array<std::byte, 24> b{};
    b.fill(v);
    return b;
}
}   // namespace flash_region_test

using namespace flash_region_test;

int main() {
    using Kvasir::Test::test;

    {
        test("bounds: read and program past the end refused, nothing asked of the chip");
        Split::reset();
        std::array<std::byte, 8> out{};
        CHECK(!Split::read(250, out) && !Split::program(250, bytes(std::byte{0}))
              && Split::calls.empty());
        CHECK(Split::read(248, out) && Split::calls.size() == 1 && Split::calls[0].what == 'r');
    }

    {
        test("split: 24 bytes from 8 are two whole pages, the rest 0xFF");
        Split::reset();
        CHECK(Split::program(8, bytes(std::byte{0x00})));
        CHECK(Split::calls.size() == 2 && Split::calls[0].at == 0 && Split::calls[1].at == 16);
        CHECK(Split::mem[7] == std::byte{0xFF} && Split::mem[8] == std::byte{0}
              && Split::mem[31] == std::byte{0} && Split::mem[32] == std::byte{0xFF});
        CHECK(Split::program(40, std::span<std::byte const>{})
              && Split::calls.size() == 2);   // nothing: true
    }

    {
        test(
          "refuse: across a page is false, nothing written; empty is true, nothing written; inside "
          "one page is one write");
        Refuse::reset();
        CHECK(!Refuse::program(8, bytes(std::byte{0})) && Refuse::calls.empty());
        CHECK(Refuse::program(0, std::span<std::byte const>{}) && Refuse::calls.empty());
        auto const page = bytes(std::byte{0x5A});
        CHECK(Refuse::program(16, std::span{page}.first(16)));
        CHECK(Refuse::calls.size() == 1 && Refuse::calls[0].at == 16
              && Refuse::mem[16] == std::byte{0x5A});
    }

    {
        test("erase: a sector index, past the end refused");
        Split::reset();
        CHECK(!Split::erase(4) && Split::calls.empty());
        CHECK(Split::erase(3) && Split::calls.size() == 1 && Split::calls[0].at == 3);
    }

    {
        test("outsideImage: at or past the image's end only");
        Split::end = base;
        CHECK(Split::outsideImage());
        Split::end = base - 4;
        CHECK(Split::outsideImage());
        Split::end = base + 1;
        CHECK(!Split::outsideImage());
    }

    {
        test(
          "CellStore over a FlashRegionBase region: refused over the image, untouched; working "
          "past it");

        struct Cfg : Kvasir::Flash::CellStoreDefaults {
            using Crc = Kvasir::Crc::Crc32;
        };

        using Store = Kvasir::Flash::CellStore<Split, std::uint32_t, Cfg>;
        Split::reset();
        Split::end   = base + 64;
        auto const r = Store::store(1);
        CHECK(!r && r.error() == Kvasir::Flash::CellError::overlapsImage && Split::calls.empty());
        Store::forget();
        Split::end = base;
        CHECK(Store::store(1).has_value() && Store::load() == std::optional<std::uint32_t>{1});
        CHECK(Store::store(2).has_value() && Store::load() == std::optional<std::uint32_t>{2});
    }

    return Kvasir::Test::report();
}
