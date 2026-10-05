#pragma once
// One record in NOR flash that survives a power cut at any moment: a ring of page-aligned cells (header + T's
// bytes). A store appends after the newest cell; an erase never hits the block holding the newest cell, so after any
// cut the newest valid cell is the last completed store or the one in flight.
//
//     using Store = Kvasir::Flash::CellStore<Region, Settings>;
//     Settings s{};
//     Store::load(s);                                  // false: no valid cell (s unchanged)
//     if(auto const w = Store::store(s); !w) { ... w.error() ... }
//
// A cell is 16 + sizeof(T) bytes rounded up to a page, or to whole sectors past a sector (the erase unit, a block, is
// then the cell's sectors); two blocks at least. Cell layout:
//     +0 magic u32 'KCL1'   +4 sequence u32   +8 version u16   +10 length u16
//     +12 crc u32 (Config::Crc over bytes 0..11 and the payload)   +16 payload, the rest 0xFF
// Valid: magic, sequence not 0 / ~0, length fits, CRC. Blank: all 0xFF. Anything else is torn: skipped, never written
// over. Pages are programmed last first, the header's page last.
//
// Config (derive from CellStoreDefaults): Version, CellBytes (0: as above; else exactly that, a page multiple),
// VerifyAfterWrite, SkipUnchanged, EraseAhead (N > 0: maintain() on Hook::MainLoop erases the next block when N blank
// cells are left), Crc, Legacy (a record imported when the store is empty; inside the Config `Crc` names the
// inherited engine, a firmware type of that name is `::Crc`), and optionally
//     static bool migrate(std::uint16_t version, std::uint16_t length, auto const& read, T& out);
// for a cell of another version or length; without it such a cell loads as nothing.
//
// Region (NorRegion): addresses from the region's start; an optional outsideImage() returning false makes every call
// fail with overlapsImage and touch nothing.
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/Crc.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Flash {

template<typename R>
concept NorRegion
  = requires(std::uint32_t a, std::span<std::byte> out, std::span<std::byte const> in) {
        { R::SectorBytes } -> std::convertible_to<std::uint32_t>;
        { R::PageBytes } -> std::convertible_to<std::uint32_t>;
        { R::sectorCount() } -> std::convertible_to<std::uint32_t>;
        { R::read(a, out) } -> std::same_as<bool>;
        { R::program(a, in) } -> std::same_as<bool>;
        { R::erase(a) } -> std::same_as<bool>;
    };

struct NoLegacy {};

struct CellStoreDefaults {
    static constexpr std::uint16_t Version          = 1;
    static constexpr std::size_t   CellBytes        = 0;
    static constexpr bool          VerifyAfterWrite = true;
    static constexpr bool          SkipUnchanged    = true;
    static constexpr std::size_t   EraseAhead       = 0;
    using Crc                                       = Kvasir::Crc::Crc32;
    using Legacy                                    = NoLegacy;
};

enum class CellError : std::uint8_t { flash, verify, noBlankCell, overlapsImage };

struct Written {
    std::uint32_t sequence;
    std::uint16_t block;
    std::uint16_t cell;
    bool          erased;      // this store erased a block first
    bool          unchanged;   // SkipUnchanged: equal to the newest cell, nothing written
};

struct CellStats {
    std::uint32_t sequence;   // of the newest valid cell, 0: none
    std::uint16_t block;
    std::uint16_t cell;
    std::uint16_t scanned;    // at mount: cells read
    std::uint16_t invalid;    // with the magic, but a bad CRC or header
    std::uint16_t torn;       // neither blank nor with the magic
    bool          imported;   // the newest came from Config::Legacy
};

namespace Detail {
    constexpr std::uint32_t roundUp(std::uint32_t v,
                                    std::uint32_t to) {
        return (v + to - 1) / to * to;
    }
}   // namespace Detail

template<NorRegion Region, typename T, typename Config = CellStoreDefaults>
struct CellStore {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a cell holds T's bytes: T must be trivially copyable");
    static_assert(sizeof(T) <= 0xFFFF - 16,
                  "a cell's length field is 16 bits");

    static constexpr std::uint32_t HeaderBytes = 16;
    static constexpr std::uint32_t Magic       = 0x314C'434BU;   // 'K' 'C' 'L' '1'
    static constexpr std::uint32_t SectorBytes = Region::SectorBytes;
    static constexpr std::uint32_t PageBytes   = Region::PageBytes;
    static constexpr std::uint32_t Sectors     = Region::sectorCount();
    static constexpr std::uint16_t Version     = Config::Version;

    static constexpr std::uint32_t RawCell = Config::CellBytes != 0
                                             ? static_cast<std::uint32_t>(Config::CellBytes)
                                             : HeaderBytes + static_cast<std::uint32_t>(sizeof(T));
    static_assert(RawCell >= HeaderBytes + sizeof(T),
                  "CellBytes too small for the header and T");
    // an explicit Config::CellBytes is taken as it is (and checked); the default is rounded to pages, or to sectors
    static constexpr std::uint32_t CellBytes = Config::CellBytes != 0 ? RawCell
                                             : RawCell <= SectorBytes
                                               ? Detail::roundUp(RawCell, PageBytes)
                                               : Detail::roundUp(RawCell, SectorBytes);
    static_assert(CellBytes % PageBytes == 0,
                  "CellBytes must be a page multiple: one program per page per erase");
    static constexpr std::uint32_t SectorsPerBlock
      = CellBytes <= SectorBytes ? 1 : CellBytes / SectorBytes;
    static constexpr std::uint32_t BlockBytes    = SectorsPerBlock * SectorBytes;
    static constexpr std::uint32_t CellsPerBlock = BlockBytes / CellBytes;
    static_assert(Sectors % SectorsPerBlock == 0,
                  "the region must be a whole number of blocks (a big cell's sectors)");
    static constexpr std::uint32_t Blocks = Sectors / SectorsPerBlock;
    static_assert(Blocks >= 2,
                  "the newest cell must survive the erase of a block: two blocks at least (two "
                  "sectors, or twice a "
                  "big cell's sectors)");
    static constexpr std::uint32_t TotalCells = Blocks * CellsPerBlock;
    static_assert(TotalCells <= 0xFFFF,
                  "a cell index is 16 bits");

    using Crc = typename Config::Crc;
    static_assert(sizeof(typename Crc::value_type) <= 4,
                  "the cell's CRC field is 32 bits");

    /// Reads a cell's payload, for Config::migrate.
    struct PayloadReader {
        std::uint32_t cellAddress;
        std::uint16_t length;

        bool operator()(std::uint32_t        offset,
                        std::span<std::byte> out) const {
            if(std::uint64_t{offset} + out.size() > length) { return false; }
            return Region::read(cellAddress + HeaderBytes + offset, out);
        }
    };

    /// The newest valid cell into `out`. False (and `out` untouched) when there is none, or it is of another version
    /// or length and there is no migrate.
    static bool load(T& out) {
        if(!mount(&out)) { return false; }
        if(newest_ == NoCell) { return false; }
        if(newestVersion_ == Version && newestLength_ == sizeof(T)) {
            return Region::read(cellAddress(newest_) + HeaderBytes,
                                std::as_writable_bytes(std::span{std::addressof(out), 1}));
        }
        if constexpr(HasMigrate) {
            return Config::migrate(newestVersion_,
                                   newestLength_,
                                   PayloadReader{cellAddress(newest_), newestLength_},
                                   out);
        } else {
            return false;
        }
    }

    [[nodiscard]] static std::optional<T> load() {
        T v{};
        if(!load(v)) { return std::nullopt; }
        return v;
    }

    static std::expected<Written,
                         CellError>
    store(T const& v) {
        if(!mount(nullptr)) { return std::unexpected{CellError::overlapsImage}; }
        if constexpr(Config::SkipUnchanged) {
            if(newest_ != NoCell && newestVersion_ == Version && newestLength_ == sizeof(T)
               && equalsNewest(v))
            {
                return Written{newestSeq_, blockOf(newest_), cellIn(newest_), false, true};
            }
        }
        std::uint32_t const start
          = newest_ == NoCell ? firstCellAfterLegacy_ : (newest_ + 1) % TotalCells;
        return storeFrom(v, start);
    }

    /// EraseAhead: erase the next block before the ring needs it, when Config::EraseAhead blank cells are left.
    static void maintain() {
        if constexpr(Config::EraseAhead > 0) {
            if(!mounted_ || !placementOk_ || newest_ == NoCell) { return; }
            std::uint32_t const left = CellsPerBlock - 1 - cellIn(newest_);
            if(left > Config::EraseAhead) { return; }
            std::uint32_t const next = (blockOfCell(newest_) + 1) % Blocks;
            if(aheadDone_ == next) { return; }
            if(!blockBlank(next) && !eraseBlock(next)) { return; }
            aheadDone_ = next;
        }
    }

    [[nodiscard]] static CellStats stats() {
        (void)mount(nullptr);
        return stats_;
    }

    /// Forget what is known in RAM (a test's "reboot"); the next call mounts again.
    static void forget() {
        mounted_   = false;
        aheadDone_ = NoCell;
    }

    using Extends = std::conditional_t<
      (Config::EraseAhead > 0),
      brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &CellStore::maintain>>,
      brigand::list<>>;

private:
    static constexpr std::uint32_t NoCell     = 0xFFFF'FFFFU;
    static constexpr bool          HasMigrate = requires(T& o, PayloadReader const& r) {
        { Config::migrate(std::uint16_t{}, std::uint16_t{}, r, o) } -> std::same_as<bool>;
    };

    struct Header {
        std::uint32_t magic;
        std::uint32_t sequence;
        std::uint16_t version;
        std::uint16_t length;
        std::uint32_t crc;
    };

    static_assert(sizeof(Header) == HeaderBytes);

    static constexpr std::uint32_t blockOfCell(std::uint32_t c) { return c / CellsPerBlock; }

    static constexpr std::uint16_t blockOf(std::uint32_t c) {
        return static_cast<std::uint16_t>(blockOfCell(c));
    }

    static constexpr std::uint16_t cellIn(std::uint32_t c) {
        return static_cast<std::uint16_t>(c % CellsPerBlock);
    }

    static constexpr std::uint32_t cellAddress(std::uint32_t c) {
        return blockOfCell(c) * BlockBytes + (c % CellsPerBlock) * CellBytes;
    }

    static bool readHeader(std::uint32_t c,
                           Header&       h) {
        return Region::read(cellAddress(c),
                            std::as_writable_bytes(std::span{std::addressof(h), 1}));
    }

    // the CRC of a cell in flash: header bytes 0..11 and `length` payload bytes, read in small pieces
    static bool cellCrc(std::uint32_t  c,
                        Header const&  h,
                        std::uint32_t& crc) {
        Crc e{};
        e.update(std::as_bytes(std::span{std::addressof(h), 1}).first(12));
        std::array<std::byte, 64> buf{};
        std::uint32_t const       base = cellAddress(c) + HeaderBytes;
        for(std::uint32_t done = 0; done < h.length;) {
            auto const n = std::min<std::uint32_t>(buf.size(), h.length - done);
            if(!Region::read(base + done, std::span{buf}.first(n))) { return false; }
            e.update(std::span<std::byte const>{buf}.first(n));
            done += n;
        }
        crc = static_cast<std::uint32_t>(e.finish());
        return true;
    }

    static std::uint32_t valueCrc(Header const& h,
                                  T const&      v) {
        Crc e{};
        e.update(std::as_bytes(std::span{std::addressof(h), 1}).first(12));
        e.update(std::as_bytes(std::span{std::addressof(v), 1}));
        return static_cast<std::uint32_t>(e.finish());
    }

    static bool rangeBlank(std::uint32_t addr,
                           std::uint32_t bytes) {
        std::array<std::byte, 64> buf{};
        for(std::uint32_t done = 0; done < bytes;) {
            auto const n = std::min<std::uint32_t>(buf.size(), bytes - done);
            if(!Region::read(addr + done, std::span{buf}.first(n))) { return false; }
            for(std::uint32_t i = 0; i != n; ++i) {
                if(buf[i] != std::byte{0xFF}) { return false; }
            }
            done += n;
        }
        return true;
    }

    static bool cellBlank(std::uint32_t c) { return rangeBlank(cellAddress(c), CellBytes); }

    static bool blockBlank(std::uint32_t b) { return rangeBlank(b * BlockBytes, BlockBytes); }

    static bool eraseBlock(std::uint32_t b) {
        for(std::uint32_t s = 0; s != SectorsPerBlock; ++s) {
            if(!Region::erase(b * SectorsPerBlock + s)) { return false; }
        }
        return true;
    }

    // is the cell at `addr` byte-for-byte `h` followed by v?
    static bool cellEquals(std::uint32_t addr,
                           Header const& h,
                           T const&      v) {
        std::array<std::byte, 64> buf{};
        auto const                head = std::as_bytes(std::span{std::addressof(h), 1});
        auto const                body = std::as_bytes(std::span{std::addressof(v), 1});
        std::uint32_t const       all  = HeaderBytes + static_cast<std::uint32_t>(sizeof(T));
        for(std::uint32_t done = 0; done < all;) {
            auto const n = std::min<std::uint32_t>(buf.size(), all - done);
            if(!Region::read(addr + done, std::span{buf}.first(n))) { return false; }
            for(std::uint32_t i = 0; i != n; ++i) {
                auto const at   = done + i;
                auto const want = at < HeaderBytes ? head[at] : body[at - HeaderBytes];
                if(buf[i] != want) { return false; }
            }
            done += n;
        }
        return true;
    }

    static bool equalsNewest(T const& v) {
        Header h{};
        if(!readHeader(newest_, h)) { return false; }
        return h.crc == valueCrc(h, v) && cellEquals(cellAddress(newest_), h, v);
    }

    // program cell c: its pages last first, the header's page last; pages past the payload stay blank
    static bool programCell(std::uint32_t c,
                            Header const& h,
                            T const&      v) {
        std::uint32_t const              used = HeaderBytes + static_cast<std::uint32_t>(sizeof(T));
        std::uint32_t const              pages = (used + PageBytes - 1) / PageBytes;
        auto const                       head  = std::as_bytes(std::span{std::addressof(h), 1});
        auto const                       body  = std::as_bytes(std::span{std::addressof(v), 1});
        std::array<std::byte, PageBytes> page{};
        for(std::uint32_t p = pages; p-- != 0;) {
            std::uint32_t const from = p * PageBytes;
            std::uint32_t const n    = std::min<std::uint32_t>(PageBytes, used - from);
            for(std::uint32_t i = 0; i != n; ++i) {
                auto const at = from + i;
                page[i]       = at < HeaderBytes ? head[at] : body[at - HeaderBytes];
            }
            if(!Region::program(cellAddress(c) + from, std::span<std::byte const>{page}.first(n))) {
                return false;
            }
        }
        return true;
    }

    static std::expected<Written,
                         CellError>
    storeFrom(T const&      v,
              std::uint32_t slot) {
        std::uint32_t seq         = newestSeq_ + 1;
        bool          erased      = false;
        auto const    newestBlock = newest_ == NoCell ? NoCell : blockOfCell(newest_);
        for(std::uint32_t tries = 0; tries != TotalCells; ++tries, slot = (slot + 1) % TotalCells) {
            if(cellIn(slot) == 0) {
                // a whole block, never the newest cell's: an erase cut short leaves any mix in it
                if(blockOfCell(slot) == newestBlock) { break; }
                if(!blockBlank(blockOfCell(slot))) {
                    if(!eraseBlock(blockOfCell(slot))) { return std::unexpected{CellError::flash}; }
                    erased = true;
                }
            } else if(!cellBlank(slot)) {
                continue;   // torn: never written over
            }
            if(seq == 0 || seq == NoCell) { seq = 1; }
            Header h{Magic, seq, Version, static_cast<std::uint16_t>(sizeof(T)), 0};
            h.crc = valueCrc(h, v);
            if(!programCell(slot, h, v)) { return std::unexpected{CellError::flash}; }
            if constexpr(Config::VerifyAfterWrite) {
                if(!cellEquals(cellAddress(slot), h, v)) {
                    ++seq;   // a sequence is never programmed twice
                    continue;
                }
            }
            if(blockOfCell(slot) == aheadDone_) {
                aheadDone_ = NoCell;
            }   // in use again: erase it next time round
            newest_         = slot;
            newestSeq_      = seq;
            newestVersion_  = Version;
            newestLength_   = static_cast<std::uint16_t>(sizeof(T));
            stats_.sequence = seq;
            stats_.block    = blockOf(slot);
            stats_.cell     = cellIn(slot);
            return Written{seq, blockOf(slot), cellIn(slot), erased, false};
        }
        return std::unexpected{CellError::noBlankCell};
    }

    // read every header; the valid one with the highest sequence is the newest. `importInto`: where a legacy record
    // goes when there is no cell (load), nullptr: a scratch value (store, stats)
    static bool mount(T* importInto) {
        if(mounted_) { return placementOk_; }
        mounted_              = true;
        stats_                = {};
        newest_               = NoCell;
        newestSeq_            = 0;
        firstCellAfterLegacy_ = 0;
        if constexpr(requires {
                         { Region::outsideImage() } -> std::same_as<bool>;
                     })
        {
            placementOk_ = Region::outsideImage();
            if(!placementOk_) {
#ifdef USE_UC_LOG
                UC_LOG_C(
                  "cell store: the region overlaps the firmware image - nothing is read or "
                  "written");
#endif
                return false;
            }
        }
        for(std::uint32_t c = 0; c != TotalCells; ++c) {
            Header h{};
            if(!readHeader(c, h)) { continue; }
            ++stats_.scanned;
            if(h.magic != Magic) {
                if(h.magic != 0xFFFF'FFFFU || !cellBlank(c)) { ++stats_.torn; }
                continue;
            }
            std::uint32_t crc = 0;
            if(h.sequence == 0 || h.sequence == NoCell || h.length + HeaderBytes > CellBytes
               || !cellCrc(c, h, crc) || crc != h.crc)
            {
                ++stats_.invalid;
                continue;
            }
            if(newest_ == NoCell || h.sequence > newestSeq_) {
                newest_        = c;
                newestSeq_     = h.sequence;
                newestVersion_ = h.version;
                newestLength_  = h.length;
            }
        }
        if(newest_ != NoCell) {
            stats_.sequence = newestSeq_;
            stats_.block    = blockOf(newest_);
            stats_.cell     = cellIn(newest_);
            return true;
        }
        if constexpr(!std::is_same_v<typename Config::Legacy, NoLegacy>) {
            using L = typename Config::Legacy;
            // the import never erases the legacy bytes before it has a copy: it starts in the block after theirs
            if constexpr(requires { L::template sectorIn<Region>(); }) {
                constexpr auto s = L::template sectorIn<Region>();
                if constexpr(s.has_value()) {
                    firstCellAfterLegacy_ = ((*s / SectorsPerBlock + 1) % Blocks) * CellsPerBlock;
                }
            }
            if(importInto != nullptr && L::read(*importInto)) {
                if(storeFrom(*importInto, firstCellAfterLegacy_)) { stats_.imported = true; }
            }
        }
        return true;
    }

    static inline bool          mounted_{};
    static inline bool          placementOk_{true};
    static inline std::uint32_t aheadDone_{NoCell};   // EraseAhead: the block erased ahead
    static inline std::uint32_t newest_{NoCell};
    static inline std::uint32_t newestSeq_{};
    static inline std::uint16_t newestVersion_{};
    static inline std::uint16_t newestLength_{};
    static inline std::uint32_t firstCellAfterLegacy_{};
    static inline CellStats     stats_{};
};

/// SimpleEeprom's API over a CellStore: value() loads once (T{} when nothing), writeValue() stores the RAM copy (true
/// when it wrote).
template<typename Store, typename T = std::remove_cvref_t<decltype(*Store::load())>>
struct CellEeprom {
    static T& value() {
        if(!read_) {
            if(!Store::load(ram_)) { ram_ = T{}; }
            read_ = true;
        }
        return ram_;
    }

    static bool writeValue() {
        auto const w = Store::store(value());
        if(!w) {
#ifdef USE_UC_LOG
            UC_LOG_E("cell store: not stored ({})", static_cast<unsigned>(w.error()));
#endif
            return false;
        }
        return !w->unchanged;
    }

private:
    static inline T    ram_{};
    static inline bool read_{};
};
}   // namespace Kvasir::Flash
