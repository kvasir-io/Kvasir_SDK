#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// NOR flash in RAM with power cuts, in the shape Kvasir::Flash::NorRegion takes (static functions; one flash per
/// Tag): erase sets a sector to 0xFF, program can only clear bits (a 0 -> 1 it is asked for is counted as an
/// overprogram, a store bug, and not done). cutAfter(n): the operation after n completed ones does a random part of
/// its work - a prefix of a program with a noisy last byte, a part of an erase with noise at its edge - and every
/// later call fails until powerOn(). The model of fs/test/Ram.hpp's RamFlash, in Kvasir's terms.
namespace Kvasir::Test {

template<typename Tag, std::uint32_t SectorBytesV, std::uint32_t PageBytesV, std::uint32_t SectorsV>
struct FakeNorFlash {
    static constexpr std::uint32_t SectorBytes = SectorBytesV;
    static constexpr std::uint32_t PageBytes   = PageBytesV;
    static constexpr std::uint32_t Bytes       = SectorBytesV * SectorsV;
    static constexpr std::uint32_t Pages       = Bytes / PageBytesV;

    static constexpr std::uint32_t sectorCount() { return SectorsV; }

    static inline std::array<std::byte, Bytes> mem = [] {
        std::array<std::byte, Bytes> m{};
        m.fill(std::byte{0xFF});
        return m;
    }();

    static inline std::uint64_t                    ops{};   // completed program/erase operations
    static inline std::uint64_t                    cutAt{~0ULL};   // the operation that is cut
    static inline bool                             powered{true};
    static inline std::uint32_t                    seed{0x1234'5678U};
    static inline std::uint64_t                    overprograms{};
    static inline std::uint64_t                    pageCrossings{};
    static inline std::array<std::uint32_t, Pages> pagePrograms{};   // since the page's last erase
    static inline std::array<std::uint32_t, SectorsV> erases{};
    static inline std::uint64_t                       reads{};

    static void blank() {
        mem.fill(std::byte{0xFF});
        pagePrograms.fill(0);
        erases.fill(0);
        ops = overprograms = pageCrossings = reads = 0;
        cutAt                                      = ~0ULL;
        powered                                    = true;
    }

    /// The operation after `n` completed ones (counted from now) is cut.
    static void cutAfter(std::uint64_t n) { cutAt = ops + n; }

    static void powerOn() {
        powered = true;
        cutAt   = ~0ULL;
    }

    [[nodiscard]] static std::uint32_t maxPagePrograms() {
        return *std::max_element(pagePrograms.begin(), pagePrograms.end());
    }

    static bool read(std::uint32_t        addr,
                     std::span<std::byte> out) {
        if(!powered || std::uint64_t{addr} + out.size() > Bytes) { return false; }
        ++reads;
        std::copy_n(mem.begin() + addr, out.size(), out.begin());
        return true;
    }

    static bool program(std::uint32_t              addr,
                        std::span<std::byte const> in) {
        if(!powered || std::uint64_t{addr} + in.size() > Bytes || in.empty()) { return false; }
        if(addr / PageBytes != (addr + static_cast<std::uint32_t>(in.size()) - 1) / PageBytes) {
            ++pageCrossings;
        }
        for(std::uint32_t p = addr / PageBytes; p <= (addr + in.size() - 1) / PageBytes; ++p) {
            ++pagePrograms[p];
        }
        std::size_t n   = in.size();
        bool const  cut = ops == cutAt;
        if(cut) { n = random() % (in.size() + 1); }
        for(std::size_t i = 0; i != n; ++i) { programByte(addr + i, in[i]); }
        if(cut) {
            if(n < in.size()) {   // the byte the cut came in: some of its bits
                mem[addr + n] &= in[n] | static_cast<std::byte>(random() & 0xFFU);
            }
            powered = false;
            return false;
        }
        ++ops;
        return true;
    }

    static bool erase(std::uint32_t sector) {
        if(!powered || sector >= SectorsV) { return false; }
        auto const begin = mem.begin() + sector * SectorBytes;
        if(ops == cutAt) {
            // a part erased, noise at its edge, the rest as it was
            auto const n = random() % (SectorBytes + 1);
            std::fill_n(begin, n, std::byte{0xFF});
            for(std::uint32_t i = static_cast<std::uint32_t>(n);
                i < std::min<std::uint32_t>(SectorBytes, n + 8);
                ++i)
            {
                begin[i] = static_cast<std::byte>(random());
            }
            powered = false;
            return false;
        }
        std::fill_n(begin, SectorBytes, std::byte{0xFF});
        std::fill_n(pagePrograms.begin() + sector * (SectorBytes / PageBytes),
                    SectorBytes / PageBytes,
                    0U);
        ++erases[sector];
        ++ops;
        return true;
    }

    /// Bytes of the flash, to write or damage by hand.
    static std::span<std::byte> raw(std::uint32_t addr,
                                    std::uint32_t n) {
        return std::span{mem}.subspan(addr, n);
    }

private:
    static std::uint32_t random() {
        seed = seed * 1'664'525U + 1'013'904'223U;
        return seed >> 8U;
    }

    static void programByte(std::uint32_t at,
                            std::byte     b) {
        if((~mem[at] & b) != std::byte{0}) { ++overprograms; }
        mem[at] &= b;
    }
};
}   // namespace Kvasir::Test
