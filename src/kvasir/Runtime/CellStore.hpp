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
//     +0 magic u32 'KCEL' (the validity magic: a written cell)   +4 sequence u32   +8 length u32
//     +12 crc u32 (Config::Crc over bytes 0..11 and the payload)   +16 payload, the rest 0xFF
// Valid: magic, sequence not 0 / ~0, length fits, CRC. Blank: all 0xFF. Anything else is torn: skipped, never written
// over. Pages are programmed last first, the header's page last. A cell is for the image that wrote it: nothing
// tells one image's layout of T from another's (a cell whose length is not sizeof(T) loads as nothing).
//
// Config (must derive from CellStoreDefaults, asserted): cellBytes (0: as above; else exactly that, a page multiple),
// verifyAfterWrite, skipUnchanged, eraseAhead (N > 0: turn() on Hook::MainLoop erases the next block when N blank
// cells are left), Crc (default: the image's engine for CRC-32/ISO-HDLC, Kvasir::Crc::ImageEngine - the software
// Crc32, or the chip's hardware where the image names it; inside the Config `Crc` names the inherited engine, a
// firmware type of that name is `::Crc`). A hardware engine that fails (its failed(), Runtime/CrcImageEngine.hpp):
// mount() checks that cell again with the software, and a store() writes nothing and returns CellError::crc (CellEeprom
// retries it as any other refusal).
//
// Region (Hal::NorRegion, kvasir/Hal/Flash.hpp; a chip's region derives from Hal::FlashRegionBase): addresses from the
// region's start; an optional outsideImage() returning false makes every call fail with overlapsImage and touch
// nothing.
#include "kvasir/Hal/Flash.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Runtime/CrcImageEngine.hpp"   // ImageEngine and the software engines (Util/Crc.hpp)
#include "kvasir/Runtime/Store.hpp"
#include "kvasir/Util/Result.hpp"
#include "kvasir/Util/Retry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Flash {

struct CellStoreDefaults {
    static constexpr std::size_t cellBytes        = 0;
    static constexpr bool        verifyAfterWrite = true;
    static constexpr bool        skipUnchanged    = true;
    static constexpr std::size_t eraseAhead       = 0;
    using Crc = Kvasir::Crc::ImageEngine<Kvasir::Crc::Presets::crc32IsoHdlc>;
};

// crc: the CRC engine failed (a hardware engine's transfer timed out or hit a bus error) - nothing was written
enum class CellError : std::uint8_t { flash, verify, noBlankCell, overlapsImage, crc };

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
    std::uint16_t scanned;   // at mount: cells read
    std::uint16_t invalid;   // with the magic, but a bad CRC or header
    std::uint16_t torn;      // neither blank nor with the magic
};

namespace Detail {
    constexpr std::uint32_t roundUp(std::uint32_t v,
                                    std::uint32_t to) {
        return (v + to - 1) / to * to;
    }
}   // namespace Detail

template<Hal::NorRegion Region, typename T, typename Config = CellStoreDefaults>
struct CellStore {
    // A config sets only what differs and inherits the rest: one that does not derive would have to repeat every
    // member, and one it forgot would fall back silently.
    static_assert(std::derived_from<Config,
                                    CellStoreDefaults>,
                  "a CellStore config derives from Kvasir::Flash::CellStoreDefaults");
    static_assert(std::is_trivially_copyable_v<T>,
                  "a cell holds T's bytes: T must be trivially copyable");

    static constexpr std::uint32_t headerBytes = 16;
    static constexpr std::uint32_t magic       = 0x4C45'434BU;   // 'K' 'C' 'E' 'L'
    static constexpr std::uint32_t sectorBytes = Region::sectorBytes;
    static constexpr std::uint32_t pageBytes   = Region::pageBytes;
    static constexpr std::uint32_t sectors     = Region::sectorCount();

    static constexpr std::uint32_t rawCell = Config::cellBytes != 0
                                             ? static_cast<std::uint32_t>(Config::cellBytes)
                                             : headerBytes + static_cast<std::uint32_t>(sizeof(T));
    static_assert(rawCell >= headerBytes + sizeof(T),
                  "CellBytes too small for the header and T");
    // an explicit Config::cellBytes is taken as it is (and checked); the default is rounded to pages, or to sectors
    static constexpr std::uint32_t cellBytes = Config::cellBytes != 0 ? rawCell
                                             : rawCell <= sectorBytes
                                               ? Detail::roundUp(rawCell, pageBytes)
                                               : Detail::roundUp(rawCell, sectorBytes);
    static_assert(cellBytes % pageBytes == 0,
                  "CellBytes must be a page multiple: one program per page per erase");
    static constexpr std::uint32_t sectorsPerBlock
      = cellBytes <= sectorBytes ? 1 : cellBytes / sectorBytes;
    static constexpr std::uint32_t blockBytes    = sectorsPerBlock * sectorBytes;
    static constexpr std::uint32_t cellsPerBlock = blockBytes / cellBytes;
    static_assert(sectors % sectorsPerBlock == 0,
                  "the region must be a whole number of blocks (a big cell's sectors)");
    static constexpr std::uint32_t blocks = sectors / sectorsPerBlock;
    static_assert(blocks >= 2,
                  "the newest cell must survive the erase of a block: two blocks at least (two "
                  "sectors, or twice a "
                  "big cell's sectors)");
    static constexpr std::uint32_t totalCells = blocks * cellsPerBlock;
    static_assert(totalCells <= 0xFFFF,
                  "a cell index is 16 bits");

    using Crc = typename Config::Crc;
    static_assert(sizeof(typename Crc::value_type) <= 4,
                  "the cell's CRC field is 32 bits");

    /// The newest valid cell into `out`. False (and `out` untouched) when there is none, or its length is not
    /// sizeof(T).
    static bool load(T& out) {
        if(!mount()) { return false; }
        if(newest_ == noCell) { return false; }
        if(newestLength_ != sizeof(T)) { return false; }
        return Region::read(cellAddress(newest_) + headerBytes,
                            std::as_writable_bytes(std::span{std::addressof(out), 1}));
    }

    [[nodiscard]] static std::optional<T> load() {
        T v{};
        if(!load(v)) { return std::nullopt; }
        return v;
    }

    static Kvasir::Result<Written,
                          CellError>
    store(T const& v) {
        if(!mount()) { return std::unexpected{CellError::overlapsImage}; }
        if constexpr(Config::skipUnchanged) {
            if(newest_ != noCell && newestLength_ == sizeof(T) && equalsNewest(v)) {
                return Written{newestSeq_, blockOf(newest_), cellIn(newest_), false, true};
            }
        }
        std::uint32_t const start = newest_ == noCell ? 0 : (newest_ + 1) % totalCells;
        return storeFrom(v, start);
    }

    /// EraseAhead: erase the next block before the ring needs it, when Config::eraseAhead blank cells are left.
    static void turn() {
        if constexpr(Config::eraseAhead > 0) {
            if(!mounted_ || !placementOk_ || newest_ == noCell) { return; }
            std::uint32_t const left = cellsPerBlock - 1 - cellIn(newest_);
            if(left > Config::eraseAhead) { return; }
            std::uint32_t const next = (blockOfCell(newest_) + 1) % blocks;
            if(aheadDone_ == next) { return; }
            if(!blockBlank(next) && !eraseBlock(next)) { return; }
            aheadDone_ = next;
        }
    }

    [[nodiscard]] static CellStats stats() {
        (void)mount();
        return stats_;
    }

    /// Forget what is known in RAM (a test's "reboot"); the next call mounts again.
    static void forget() {
        mounted_   = false;
        aheadDone_ = noCell;
    }

    using Extends = std::conditional_t<
      (Config::eraseAhead > 0),
      brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &CellStore::turn>>,
      brigand::list<>>;

private:
    static constexpr std::uint32_t noCell = 0xFFFF'FFFFU;

    struct Header {
        std::uint32_t magic;
        std::uint32_t sequence;
        std::uint32_t length;
        std::uint32_t crc;
    };

    static_assert(sizeof(Header) == headerBytes);

    static constexpr std::uint32_t blockOfCell(std::uint32_t c) { return c / cellsPerBlock; }

    static constexpr std::uint16_t blockOf(std::uint32_t c) {
        return static_cast<std::uint16_t>(blockOfCell(c));
    }

    static constexpr std::uint16_t cellIn(std::uint32_t c) {
        return static_cast<std::uint16_t>(c % cellsPerBlock);
    }

    static constexpr std::uint32_t cellAddress(std::uint32_t c) {
        return blockOfCell(c) * blockBytes + (c % cellsPerBlock) * cellBytes;
    }

    static bool readHeader(std::uint32_t c,
                           Header&       h) {
        return Region::read(cellAddress(c),
                            std::as_writable_bytes(std::span{std::addressof(h), 1}));
    }

    // the CRC of a cell in flash: header bytes 0..11 and `length` payload bytes, read in small pieces; false when a
    // read fails. An engine whose hardware failed is asked again of the software (Engine<params> with its default
    // table: for CRC-32 the one Persistent's records already carry): skipped as unreadable, the newest cell would leave
    // an older one loaded, and the next store would reuse its sequence or erase the block that holds it.
    static bool cellCrc(std::uint32_t  c,
                        Header const&  h,
                        std::uint32_t& crc) {
        auto const over = [&](auto& e) {
            e.update(std::as_bytes(std::span{std::addressof(h), 1}).first(12));
            // word-aligned: a hardware engine moves whole words in one transfer
            alignas(4) std::array<std::byte, 64> buf{};
            std::uint32_t const                  base = cellAddress(c) + headerBytes;
            for(std::uint32_t done = 0; done < h.length;) {
                auto const n = std::min<std::uint32_t>(buf.size(), h.length - done);
                if(!Region::read(base + done, std::span{buf}.first(n))) { return false; }
                e.update(std::span<std::byte const>{buf}.first(n));
                done += n;
            }
            return true;
        };
        Crc e{};
        if(!over(e)) { return false; }
        if constexpr(canFail) {
            if(e.failed()) {
                static_assert(
                  requires { Crc::params; },
                  "Config::Crc: an engine that can fail (failed()) names its Params (params) - "
                  "mount() "
                  "checks a cell again with the software");
                Kvasir::Crc::Engine<Crc::params> s{};
                if(!over(s)) { return false; }
                crc = static_cast<std::uint32_t>(s.finish());
                return true;
            }
        }
        crc = static_cast<std::uint32_t>(e.finish());
        return true;
    }

    // whether Config::Crc has a failed() (Runtime/CrcImageEngine.hpp); one without it (Kvasir::Crc::Engine) never fails
    static constexpr bool canFail = requires(Crc const& e) {
        { e.failed() } -> std::convertible_to<bool>;
    };

    // the CRC a cell of `h` and `v` gets; false when the engine failed
    static bool valueCrc(Header const&  h,
                         T const&       v,
                         std::uint32_t& crc) {
        Crc e{};
        e.update(std::as_bytes(std::span{std::addressof(h), 1}).first(12));
        e.update(std::as_bytes(std::span{std::addressof(v), 1}));
        if constexpr(canFail) {
            if(e.failed()) { return false; }
        }
        crc = static_cast<std::uint32_t>(e.finish());
        return true;
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

    static bool cellBlank(std::uint32_t c) { return rangeBlank(cellAddress(c), cellBytes); }

    static bool blockBlank(std::uint32_t b) { return rangeBlank(b * blockBytes, blockBytes); }

    static bool eraseBlock(std::uint32_t b) {
        for(std::uint32_t s = 0; s != sectorsPerBlock; ++s) {
            if(!Region::erase(b * sectorsPerBlock + s)) { return false; }
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
        std::uint32_t const       all  = headerBytes + static_cast<std::uint32_t>(sizeof(T));
        for(std::uint32_t done = 0; done < all;) {
            auto const n = std::min<std::uint32_t>(buf.size(), all - done);
            if(!Region::read(addr + done, std::span{buf}.first(n))) { return false; }
            for(std::uint32_t i = 0; i != n; ++i) {
                auto const at   = done + i;
                auto const want = at < headerBytes ? head[at] : body[at - headerBytes];
                if(buf[i] != want) { return false; }
            }
            done += n;
        }
        return true;
    }

    static bool equalsNewest(T const& v) {
        Header h{};
        if(!readHeader(newest_, h)) { return false; }
        std::uint32_t crc = 0;
        return valueCrc(h, v, crc) && h.crc == crc && cellEquals(cellAddress(newest_), h, v);
    }

    // program cell c: its pages last first, the header's page last; pages past the payload stay blank
    static bool programCell(std::uint32_t c,
                            Header const& h,
                            T const&      v) {
        std::uint32_t const              used = headerBytes + static_cast<std::uint32_t>(sizeof(T));
        std::uint32_t const              pages = (used + pageBytes - 1) / pageBytes;
        auto const                       head  = std::as_bytes(std::span{std::addressof(h), 1});
        auto const                       body  = std::as_bytes(std::span{std::addressof(v), 1});
        std::array<std::byte, pageBytes> page{};
        for(std::uint32_t p = pages; p-- != 0;) {
            std::uint32_t const from = p * pageBytes;
            std::uint32_t const n    = std::min<std::uint32_t>(pageBytes, used - from);
            for(std::uint32_t i = 0; i != n; ++i) {
                auto const at = from + i;
                page[i]       = at < headerBytes ? head[at] : body[at - headerBytes];
            }
            if(!Region::program(cellAddress(c) + from, std::span<std::byte const>{page}.first(n))) {
                return false;
            }
        }
        return true;
    }

    static Kvasir::Result<Written,
                          CellError>
    storeFrom(T const&      v,
              std::uint32_t slot) {
        std::uint32_t seq         = newestSeq_ + 1;
        bool          erased      = false;
        auto const    newestBlock = newest_ == noCell ? noCell : blockOfCell(newest_);
        for(std::uint32_t tries = 0; tries != totalCells; ++tries, slot = (slot + 1) % totalCells) {
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
            if(seq == 0 || seq == noCell) { seq = 1; }
            Header h{magic, seq, static_cast<std::uint32_t>(sizeof(T)), 0};
            if(!valueCrc(h, v, h.crc)) { return std::unexpected{CellError::crc}; }
            if(!programCell(slot, h, v)) { return std::unexpected{CellError::flash}; }
            if constexpr(Config::verifyAfterWrite) {
                if(!cellEquals(cellAddress(slot), h, v)) {
                    ++seq;   // a sequence is never programmed twice
                    continue;
                }
            }
            if(blockOfCell(slot) == aheadDone_) {
                aheadDone_ = noCell;
            }   // in use again: erase it next time round
            newest_         = slot;
            newestSeq_      = seq;
            newestLength_   = static_cast<std::uint32_t>(sizeof(T));
            stats_.sequence = seq;
            stats_.block    = blockOf(slot);
            stats_.cell     = cellIn(slot);
            return Written{seq, blockOf(slot), cellIn(slot), erased, false};
        }
        return std::unexpected{CellError::noBlankCell};
    }

    // read every header; the valid one with the highest sequence is the newest
    static bool mount() {
        if(mounted_) { return placementOk_; }
        mounted_   = true;
        stats_     = {};
        newest_    = noCell;
        newestSeq_ = 0;
        if constexpr(requires {
                         { Region::outsideImage() } -> std::same_as<bool>;
                     })
        {
            placementOk_ = Region::outsideImage();
            if(!placementOk_) {
#ifdef USE_UC_LOG
                UC_LOG_E(   // the store is off for good (class L, Runtime/Check.hpp)
                  "cell store: the region overlaps the firmware image - nothing is read or "
                  "written");
#endif
                return false;
            }
        }
        for(std::uint32_t c = 0; c != totalCells; ++c) {
            Header h{};
            if(!readHeader(c, h)) { continue; }
            ++stats_.scanned;
            if(h.magic != magic) {
                if(h.magic != 0xFFFF'FFFFU || !cellBlank(c)) { ++stats_.torn; }
                continue;
            }
            std::uint32_t crc = 0;
            if(h.sequence == 0 || h.sequence == noCell || h.length > cellBytes - headerBytes
               || !cellCrc(c, h, crc) || crc != h.crc)
            {
                ++stats_.invalid;
                continue;
            }
            if(newest_ == noCell || h.sequence > newestSeq_) {
                newest_       = c;
                newestSeq_    = h.sequence;
                newestLength_ = h.length;
            }
        }
        if(newest_ != noCell) {
            stats_.sequence = newestSeq_;
            stats_.block    = blockOf(newest_);
            stats_.cell     = cellIn(newest_);
        }
        return true;
    }

    static inline bool          mounted_{};
    static inline bool          placementOk_{true};
    static inline std::uint32_t aheadDone_{noCell};   // EraseAhead: the block erased ahead
    static inline std::uint32_t newest_{noCell};
    static inline std::uint32_t newestSeq_{};
    static inline std::uint32_t newestLength_{};
    static inline CellStats     stats_{};
};

/// A RAM copy over a CellStore: value() loads once (T{} when nothing), writeValue() stores the RAM copy (true
/// when it wrote). A store that fails keeps the value pending, and turn() (Hook::MainLoop: list the CellEeprom in the
/// Startup list) stores it again after a pause that doubles up to 10 min (Kvasir::RetryingStore, Runtime/Store.hpp)
/// until it is in.
template<typename Store, typename Clock, typename T = std::remove_cvref_t<decltype(*Store::load())>>
struct CellEeprom : RetryingStore<CellEeprom<Store, Clock, T>, Clock, CellError> {
    using Base = RetryingStore<CellEeprom, Clock, CellError>;

    static T& value() {
        if(!read_) {
            if(!Store::load(ram_)) { ram_ = T{}; }
            read_ = true;
        }
        return ram_;
    }

    static bool writeValue() {
        auto const w = Store::store(value());
        if(!Base::settle(w)) { return false; }
        return !w->unchanged;
    }

    static bool retryNow() { return writeValue(); }

    /// Forget what is known in RAM (a test's "reboot").
    static void forget() {
        read_ = false;
        Base::resetRetry();
    }

    template<typename Retry,
             typename Duration>
    static void logFailed([[maybe_unused]] Retry const& r,
                          [[maybe_unused]] CellError    e,
                          [[maybe_unused]] Duration     untilNext) {
#ifdef USE_UC_LOG
        UC_LOG_E("cell store: not stored ({}, failure {}, retry in {})",
                 e,
                 r.failures,
                 std::chrono::duration_cast<std::chrono::milliseconds>(untilNext));
#endif
    }

    template<typename Retry>
    static void logRecovered([[maybe_unused]] Retry const& r) {
#ifdef USE_UC_LOG
        UC_LOG_I("cell store: stored after {} failures", r.failures);
#endif
    }

private:
    static inline T    ram_{};
    static inline bool read_{};
};
}   // namespace Kvasir::Flash
