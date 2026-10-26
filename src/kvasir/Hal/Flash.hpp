#pragma once
// A NOR flash range as the stores see it (Kvasir::Hal::NorRegion), and the one base every chip's region derives
// from (Kvasir::Hal::FlashRegionBase): the bounds checks, a write split into pages (or refused across a page) and the
// image check, written once. A chip's region supplies only what touches its controller:
//
//     template<std::uint32_t Base, std::uint32_t Sectors>
//     struct Region : Kvasir::Hal::FlashRegionBase<Region<Base, Sectors>, {Base, sectorBytes, pageBytes, Sectors},
//                                                  Kvasir::Hal::PageSplit::split> {
//         // addresses from the region's start; `page` is one whole page, the bytes not asked for 0xFF
//         static bool programPage(std::uint32_t pageAddr, std::span<std::byte const, pageBytes> page);
//         static bool eraseSector(std::uint32_t sector);
//         static void readAt(std::uint32_t addr, std::span<std::byte> out);
//         // optional: static bool outsideImage();   hides the default (another address space, partitions)
//     };
//
// A program writes whole pages: a 0xFF byte programs nothing on NOR flash, so the padding leaves what is there. An
// empty program writes nothing and is true under either PageSplit: there was nothing to do.
// outsideImage() is true when the region starts at or past the end of the firmware's flash image
// (_LINKER_image_end_flash_, every section the linker loads into flash); CellStore refuses a region where it is false.
#include "kvasir/Port/LinkerSymbols.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Kvasir::Hal {

template<typename R>
concept NorRegion = requires(std::uint32_t              a,
                             std::uint32_t              sector,
                             std::span<std::byte>       out,
                             std::span<std::byte const> in) {
    { R::sectorBytes } -> std::convertible_to<std::uint32_t>;
    { R::pageBytes } -> std::convertible_to<std::uint32_t>;
    { R::sectorCount() } -> std::convertible_to<std::uint32_t>;
    { R::read(a, out) } -> std::same_as<bool>;
    { R::program(a, in) } -> std::same_as<bool>;
    { R::erase(sector) } -> std::same_as<bool>;
};

/// Where a region is and how it is cut: `base` its first byte in the CPU's address space (what outsideImage()
/// compares), a sector the erase unit, a page the program unit.
struct FlashGeometry {
    std::uint32_t base;
    std::uint32_t sectorBytes;
    std::uint32_t pageBytes;
    std::uint32_t sectors;
};

/// A program() across a page boundary: refused (false, nothing written), or split into one page write per page.
/// An empty program() is true under both.
enum class PageSplit : bool { refuse, split };

template<typename Derived, FlashGeometry G, PageSplit Split>
struct FlashRegionBase {
    static_assert(G.sectors >= 1,
                  "a region has a sector at least");
    static_assert(G.pageBytes != 0 && G.sectorBytes % G.pageBytes == 0,
                  "a sector is whole pages");

    static constexpr std::uint32_t sectorBytes = G.sectorBytes;
    static constexpr std::uint32_t pageBytes   = G.pageBytes;
    static constexpr std::uint32_t bytes       = G.sectors * G.sectorBytes;

    [[nodiscard]] static constexpr std::uint32_t sectorCount() { return G.sectors; }

    static bool read(std::uint32_t        addr,
                     std::span<std::byte> out) {
        if(std::uint64_t{addr} + out.size() > bytes) { return false; }
        Derived::readAt(addr, out);
        return true;
    }

    static bool program(std::uint32_t              addr,
                        std::span<std::byte const> in) {
        if(std::uint64_t{addr} + in.size() > bytes) { return false; }
        if constexpr(Split == PageSplit::refuse) {
            if(!in.empty() && addr / pageBytes != (addr + in.size() - 1) / pageBytes) {
                return false;
            }
        }
        while(!in.empty()) {
            auto const page = addr / pageBytes * pageBytes;
            auto const at   = addr - page;
            auto const n    = std::min<std::size_t>(pageBytes - at, in.size());
            alignas(4) std::array<std::byte, pageBytes> buf{};
            buf.fill(std::byte{0xFF});
            std::copy_n(in.begin(), n, buf.begin() + static_cast<std::ptrdiff_t>(at));
            if(!Derived::programPage(page, std::span<std::byte const, pageBytes>{buf})) {
                return false;
            }
            addr += static_cast<std::uint32_t>(n);
            in = in.subspan(n);
        }
        return true;
    }

    static bool erase(std::uint32_t sector) {
        if(sector >= G.sectors) { return false; }
        return Derived::eraseSector(sector);
    }

    /// The end of the firmware's flash image (a Derived may hide it, a host test does).
    [[nodiscard]] static std::uint32_t imageEnd() {
        return Kvasir::Startup::linkerAddress<_LINKER_image_end_flash_>();
    }

    /// The region starts at or past the end of the image.
    [[nodiscard]] static bool outsideImage() { return G.base >= Derived::imageEnd(); }
};
}   // namespace Kvasir::Hal
