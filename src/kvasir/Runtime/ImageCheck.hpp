#pragma once
// A runtime check that the code in flash is the code that was flashed: a bounded chunk of a CRC-32 per main-loop
// turn over exactly the bytes the build put into the flash hex, compared at the end of each pass with the value the
// build wrote into the image (ImageDescriptor.hpp, cmake/tools/patch_image_crc.py).
//
// A RAM image (RAM_ONLY, or any linker file that loads the whole image into SRAM) is checked where it runs: the
// patcher then covers its read-only part - vectors, code, constants, the RAM functions - and leaves .data out. The
// same checker walks it; a stray write into the code is what it finds there. Its backend is Software<> with the
// default reader: the chips' flash readers and stream backends (RP: Flash::UncachedRead, RpStream) address the flash
// window, not SRAM.
//
//     kvasir_executable_variants(fw ... IMAGE_CRC)           # CMake: the descriptor and its post-link step
//
//     using ImageCheck = Kvasir::ImageCheck::Checker<Kvasir::ImageCheck::Software<>,
//                                                    Kvasir::ImageCheck::Paced<HW::SystickClock, 10, 1024>>;
//     using Startup = Kvasir::Startup::Startup<..., ImageCheck, ...>;     // its MainLoop hook does the work
//     ImageCheck::passes(); ImageCheck::lastResult();                       // what the application can ask
//
// What a mismatch does is the image's choice, the late hook MismatchReaction (the table in kvasir/Port/Hooks.hpp;
// default OnMismatch::Log: one error line per pass, the run goes on - not a fatal path):
//
//     template<>
//     struct Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User> {
//         using type = Kvasir::ImageCheck::OnMismatch::Panic;
//     };
//
// OnMismatch::Panic raises Panic::Cause::imageCorrupt with the computed CRC as the detail; OnMismatch::PanicAfter<N>
// logs each corrupt pass and raises it at the N-th in a row - unless a debugger has halting debug enabled (its software
// breakpoints are writes into the code) - the fatal pass starts the fatal path before its line; OnMismatch::Call<&fn>
// calls fn(got, want), or fn(got, want, inARow); OnMismatch::Count counts the corrupt passes (`mismatches`), no line.
//
// Backends: Software<TableSize, Reader> (every chip; on RP read through the uncached alias,
// Kvasir::Flash::UncachedRead, so the check neither evicts hot code from the XIP cache nor reads a cached copy), and
// the chips' hardware ones (RP: RpStream in chip/ImageCheck.hpp, SAM: SamDsu). A chunk is finished before turn()
// returns; a flash writer on the OTHER core must stop this checker first (FlashRegion.hpp's rule).
//
// Pacing: EveryTurn<ChunkBytes> (one chunk every turn), Paced<Clock, PeriodMs, ChunkBytes> (one chunk per period).
//
// Test hook (KVASIR_IMAGE_CRC_TEST): Test::corruptNextPass(address) makes the Software backend read that one byte
// flipped, once; Test::corruptPasses(address, n) for the next n passes (a stray write that stays, then is repaired).
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Register/Memory.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/Util/Crc.hpp"
#include "kvasir/Util/ImageDescriptor.hpp"
#include "kvasir/Util/PassCount.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#ifdef __arm__
    #include "cortex_common/Debug.hpp"   // Core::haltingDebug(): DHCSR through the core package's registers
#endif

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::ImageCheck {
enum class Result : std::uint8_t { unknown, ok, corrupt, noDescriptor };

[[nodiscard]] constexpr std::string_view name(Result r) {
    switch(r) {
    case Result::unknown:      return "unknown";
    case Result::ok:           return "ok";
    case Result::corrupt:      return "corrupt";
    case Result::noDescriptor: return "no descriptor";
    }
    return "?";
}

/// Whether a debugger has halting debug enabled: the core's `Core::haltingDebug()` (cortex_common/Debug.hpp; the
/// sources are written there) is `on`. A core that cannot tell (`unknown`: the Cortex-M0+, the Cortex-M4) says no here
/// - a chip package whose core documents the read can give PanicAfter a Debugger of its own.
///
/// Measured on the RP2350 over J-Link: software reads the bit as the probe
/// does (DHCSR 0x01100001) - except while the log printer's session starts: after it loaded and started an image
/// the bit was set at the first instruction, clear from 4..14 ms, set again from 74..99 ms and then stayed. A
/// corrupt pass in that gap is judged as if nobody were attached.
struct CoreDebug {
    /// Whether this core can ask at all; false: attached() is always false.
#if KVASIR_CORE_HAS_DEBUGEN_READ
    static constexpr bool readable = true;
#else
    static constexpr bool readable = false;
#endif

    [[nodiscard]] static bool attached() {
#ifdef __arm__
        return Kvasir::Core::haltingDebug() == Kvasir::Core::HaltingDebug::on;   // DHCSR.C_DEBUGEN
#else
        return false;   // the host: no core
#endif
    }
};

namespace OnMismatch {
    /// One error line per corrupt pass (class L, Runtime/Check.hpp); the run goes on. Not a fatal path: no
    /// Panic::beginFatal() (that would switch the outputs off and leave the log waiting for the host for the rest of
    /// the run).
    struct Log {
        static void mismatch([[maybe_unused]] std::uint32_t got,
                             [[maybe_unused]] std::uint32_t want) {
#ifdef USE_UC_LOG
            UC_LOG_E("image check: CRC 0x{:08x}, the image was built with 0x{:08x}: image corrupt",
                     got,
                     want);
#endif
        }
    };

    /// Nothing: the pass is corrupt (Checker::lastResult(), consecutiveCorrupt()) and the run goes on without a line.
    struct Ignore {
        static void mismatch(std::uint32_t,
                             std::uint32_t) {}
    };

    struct Panic {
        [[noreturn]] static void mismatch(std::uint32_t got,
                                          std::uint32_t) {
            Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::imageCorrupt,
                                   KVASIR_DETAIL_CALLER_PC(),
                                   got);
        }
    };

    /// Logs every corrupt pass and panics at the Limit-th in a row (an ok pass starts the count again): one bad pass
    /// is reported before the image is given up on. Not while Debugger::attached(): a debugger's software breakpoint
    /// is a write into the code, and the session should not end in a reset - the passes are still reported.
    template<std::uint32_t Limit, typename Debugger = CoreDebug>
    struct PanicAfter {
        static_assert(Limit >= 1,
                      "PanicAfter<0> would never let a pass be corrupt: use OnMismatch::Panic");
        static constexpr std::uint32_t limit = Limit;

        static void mismatch(std::uint32_t                  got,
                             [[maybe_unused]] std::uint32_t want,
                             std::uint32_t                  inARow) {
            bool const debugged = Debugger::attached();
            // the Limit-th corrupt pass in a row ends the run: its line is the fatal path's crit line
            if(inARow >= Limit && !debugged) {
#ifdef USE_UC_LOG
                KVASIR_DETAIL_FATAL_AT(
                  Kvasir::Panic::Cause::imageCorrupt,
                  KVASIR_DETAIL_CALLER_PC(),
                  got,
                  "image check: CRC 0x{:08x}, the image was built with 0x{:08x}: image corrupt "
                  "({} of {} in a row{})",
                  got,
                  want,
                  inARow,
                  Limit,
                  std::string_view{});
#else
                KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::imageCorrupt,
                                       KVASIR_DETAIL_CALLER_PC(),
                                       got);
#endif
            }
            // the passes before it (and every pass under a debugger): the run goes on, an error line (class L)
#ifdef USE_UC_LOG
            UC_LOG_E(
              "image check: CRC 0x{:08x}, the image was built with 0x{:08x}: image corrupt "
              "({} of {} in a row{})",
              got,
              want,
              inARow,
              Limit,
              debugged ? std::string_view{", debugger attached: no panic"} : std::string_view{});
#endif
        }
    };

    /// Counts the corrupt passes, no line, the run goes on: `mismatches` (the bench reads it, a field image may report
    /// it in its own way).
    struct Count {
        static inline std::uint32_t volatile mismatches{};

        static void mismatch(std::uint32_t,
                             std::uint32_t) {
            mismatches = mismatches + 1;
        }
    };

    template<auto Fn>
    struct Call {
        static void mismatch(std::uint32_t got,
                             std::uint32_t want,
                             std::uint32_t inARow) {
            if constexpr(requires { Fn(got, want, inARow); }) {
                Fn(got, want, inARow);
            } else {
                Fn(got, want);
            }
        }
    };
}   // namespace OnMismatch

// The image's reaction (header comment): type, one of OnMismatch:: or the application's own (a struct with a static
// mismatch(got, want) or mismatch(got, want, inARow)).
template<typename T, typename... Ts>
struct MismatchReaction {
    using type = OnMismatch::Log;
};

template<std::size_t ChunkBytes = 1024>
struct EveryTurn {
    static constexpr std::size_t chunkBytes = ChunkBytes;

    static bool due() { return true; }
};

template<typename Clock, std::uint32_t PeriodMs, std::size_t ChunkBytes = 1024>
struct Paced {
    static constexpr std::size_t chunkBytes = ChunkBytes;

    // the first call is due; then one period after each due one (no catch-up burst after a stall)
    static bool due() {
        auto const now = Clock::now();
        if(started_ && now < next_) { return false; }
        started_ = true;
        next_    = now + std::chrono::milliseconds{PeriodMs};
        return true;
    }

private:
    static inline typename Clock::time_point next_{};
    static inline bool                       started_{};
};

#if defined(KVASIR_IMAGE_CRC_TEST) && KVASIR_IMAGE_CRC_TEST
namespace Test {
    inline std::uintptr_t volatile corruptAt  = 0;   // 0: nothing to corrupt
    inline std::uint32_t volatile corruptLeft = 0;   // passes still to corrupt

    /// The Software backend reads the byte at `address` with its lowest bit flipped in each of the next `passes`
    /// passes (each pass reads it once), then reads it as it is again.
    inline void corruptPasses(std::uintptr_t address,
                              std::uint32_t  passes) {
        corruptLeft = passes;
        corruptAt   = passes == 0 ? 0 : address;
    }

    /// The Software backend reads the byte at `address` with its lowest bit flipped, once.
    inline void corruptNextPass(std::uintptr_t address) { corruptPasses(address, 1); }
}   // namespace Test
#endif

/// Reads the image where the descriptor says it is (the reader for a RAM image). The chips offer flash readers of
/// their own (RP: the uncached alias).
struct DirectRead {
    static std::uint32_t word(std::uintptr_t address) {
        return Kvasir::Memory::load<std::uint32_t>(address);
    }

    static std::uint8_t byte(std::uintptr_t address) {
        return Kvasir::Memory::load<std::uint8_t>(address);
    }
};

/// The CPU computes the CRC (Kvasir::Crc::Crc32T), word reads where aligned.
template<std::size_t TableSize = 16, typename Reader = DirectRead>
struct Software {
    using Engine = Kvasir::Crc::Crc32T<TableSize>;

    static void begin() { engine_ = Engine{}; }

    static void chunk(std::uintptr_t address,
                      std::size_t    length) {
        auto       a   = address;
        auto const end = address + length;
        while(a != end && (a & 3U) != 0) {
            feed(a, Reader::byte(a));
            ++a;
        }
        while(end - a >= 4) {
            auto const w = Reader::word(a);
            for(unsigned i = 0; i != 4; ++i) {
                feed(a + i,
                     static_cast<std::uint8_t>(
                       w >> (8U * i)));   // little-endian: the hex's byte order
            }
            a += 4;
        }
        while(a != end) {
            feed(a, Reader::byte(a));
            ++a;
        }
    }

    [[nodiscard]] static std::uint32_t result() { return engine_.finish(); }

private:
    [[gnu::always_inline]] static void feed([[maybe_unused]] std::uintptr_t a,
                                            std::uint8_t                    b) {
#if defined(KVASIR_IMAGE_CRC_TEST) && KVASIR_IMAGE_CRC_TEST
        if(a == Test::corruptAt) {
            b                 = static_cast<std::uint8_t>(b ^ 1U);
            auto const left   = Test::corruptLeft - 1U;
            Test::corruptLeft = left;
            if(left == 0) { Test::corruptAt = 0; }
        }
#endif
        engine_.update(std::byte{b});
    }

    static inline Engine engine_{};
};

/// The descriptor the build patched into this image, read in place (in flash, or in a RAM image where it was
/// loaded; nothing is copied).
struct LinkedDescriptor {
#if defined(KVASIR_IMAGE_CRC) && KVASIR_IMAGE_CRC
    static constexpr bool configured = true;

    static Descriptor const volatile& get() { return linkedDescriptor; }
#else
    static constexpr bool configured = false;
#endif
};

// begin() starts a pass, chunk(address, length) adds bytes (a hardware backend returns false when it could not -
// the pass is dropped and counted, not judged), result() is the CRC so far.
template<typename B>
concept Backend = requires(std::uintptr_t a, std::size_t n) {
    B::begin();
    B::chunk(a, n);
    { B::result() } -> std::same_as<std::uint32_t>;
};

namespace Detail {
    // A backend's Startup resources (kvasir/StartUp/Resources.hpp), forwarded by its Checker: a hardware backend claims
    // what it runs on (chip_rp_common's RpStream: its DMA channel and the sniffer, held across turns).
    template<typename B>
    struct BackendClaims {};

    template<typename B>
        requires requires { typename B::Claims; }
    struct BackendClaims<B> {
        using Claims = typename B::Claims;
    };

    template<typename B>
    struct BackendProvides {};

    template<typename B>
        requires requires { typename B::Provides; }
    struct BackendProvides<B> {
        using Provides = typename B::Provides;
    };

    template<Backend B>
    bool chunk(std::uintptr_t a,
               std::size_t    n) {
        if constexpr(std::is_same_v<decltype(B::chunk(a, n)), bool>) {
            return B::chunk(a, n);
        } else {
            B::chunk(a, n);
            return true;
        }
    }
}   // namespace Detail

template<Backend B, typename Pacing = EveryTurn<>, typename Source = LinkedDescriptor>
struct Checker
  : Detail::BackendClaims<B>
  , Detail::BackendProvides<B> {
    static_assert(
      Source::configured,
      "Kvasir::ImageCheck::Checker needs the image CRC descriptor: add IMAGE_CRC to the firmware's "
      "kvasir_executable_variants (or target_configure_kvasir)");

    /// One chunk (when the pacing says so); at the end of the last segment compare, count, start over.
    static void turn() {
        if(state_ == State::stopped || !Pacing::due()) { return; }
        if(state_ == State::idle && !start()) { return; }
        std::size_t budget = Pacing::chunkBytes;
        auto const& d      = Source::get();
        while(budget != 0) {
            auto const&         s      = d.segments[segment_];
            std::uint32_t const length = s.length;
            auto const          n      = std::min<std::size_t>(budget, length - offset_);
            if(!Detail::chunk<B>(static_cast<std::uintptr_t>(s.address) + offset_, n)) {
                dropPass();
                return;
            }
            budget -= n;
            offset_ += static_cast<std::uint32_t>(n);
            if(offset_ == length) {
                offset_ = 0;
                if(++segment_ == d.count) {
                    finishPass();
                    return;
                }
            }
        }
    }

    /// A whole pass at once (a boot check, a test): the result.
    static Result runPass() {
        if(state_ == State::stopped) { return last_; }
        if(state_ == State::idle && !start()) { return last_; }
        auto const& d      = Source::get();
        auto const  before = passes_.value();
        while(passes_.value() == before) {
            auto const& s = d.segments[segment_];
            if(!Detail::chunk<B>(static_cast<std::uintptr_t>(s.address) + offset_,
                                 s.length - offset_))
            {
                dropPass();
                return Result::unknown;
            }
            offset_ = 0;
            if(++segment_ == d.count) { finishPass(); }
        }
        return last_;
    }

    /// Finished passes; 0 only before the first (after 2^32 - 1 it goes on at 1: PassCount).
    [[nodiscard]] static std::uint32_t passes() { return passes_.value(); }

    [[nodiscard]] static Result lastResult() { return last_; }

    /// Passes the backend could not finish (a hardware backend's transfer that did not end in time).
    [[nodiscard]] static std::uint32_t dropped() { return dropped_; }

    /// The CRC the last finished pass computed.
    [[nodiscard]] static std::uint32_t lastCrc() { return lastCrc_; }

    /// Corrupt passes since the last ok one (0 after an ok pass).
    [[nodiscard]] static std::uint32_t consecutiveCorrupt() { return consecutiveCorrupt_; }

    /// The descriptor the checker walks.
    [[nodiscard]] static Descriptor const volatile& descriptor() { return Source::get(); }

    /// No more chunks (before a self-update rewrites the image); start() is the next step's.
    static void stop() { state_ = State::stopped; }

    /// Back to the start of a pass (after stop(), or to drop a half-done pass).
    static void restart() {
        state_   = State::idle;
        segment_ = 0;
        offset_  = 0;
    }

    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Checker::turn>;

private:
    enum class State : std::uint8_t { idle, running, stopped };

    static bool start() {
        auto const&         d     = Source::get();
        std::uint32_t const magic = d.magic;
        std::uint32_t const count = d.count;
        if(magic != Descriptor::magicValue || count == 0 || count > Descriptor::maxSegments) {
#ifdef USE_UC_LOG
            UC_LOG_W(
              "image check: no descriptor (magic 0x{:08x}): built without IMAGE_CRC's patch, or "
              "flashed from "
              "an unpatched ELF",
              magic);
#endif
            last_  = Result::noDescriptor;
            state_ = State::stopped;
            return false;
        }
#ifdef USE_UC_LOG
        if(passes_.none()) {
            std::uint32_t bytes = 0;
            for(std::size_t i = 0; i != count; ++i) { bytes += d.segments[i].length; }
            std::uint32_t const crc = d.crc;
            UC_LOG_D("image check: {} segment(s), {} bytes, crc 0x{:08x}", count, bytes, crc);
        }
#endif
        B::begin();
        segment_ = 0;
        offset_  = 0;
        state_   = State::running;
        return true;
    }

    static void dropPass() {
        ++dropped_;
        state_ = State::idle;
#ifdef USE_UC_LOG
        UC_LOG_W("image check: the backend failed a chunk, pass dropped ({} so far)", dropped_);
#endif
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    static void mismatch(std::uint32_t got) {
        using P =
          typename ::Kvasir::Port::LateT<MismatchReaction<::Kvasir::Tag::User>, Dummy...>::type;
        std::uint32_t const want = Source::get().crc;
        if constexpr(requires { P::mismatch(got, want, consecutiveCorrupt_); }) {
            P::mismatch(got, want, consecutiveCorrupt_);
        } else {
            P::mismatch(got, want);
        }
    }

    static void finishPass() {
        lastCrc_                           = B::result();
        [[maybe_unused]] auto const before = last_;
        last_ = lastCrc_ == Source::get().crc ? Result::ok : Result::corrupt;
        ++passes_;
        consecutiveCorrupt_ = last_ == Result::corrupt ? consecutiveCorrupt_ + 1 : 0;
        state_              = State::idle;
#ifdef USE_UC_LOG
        if(last_ == Result::ok && before != Result::ok) {
            UC_LOG_D("image check: pass {} ok (crc 0x{:08x})", passes_.value(), lastCrc_);
        }
#endif
        if(last_ == Result::corrupt) { mismatch(lastCrc_); }
    }

    static inline std::uint32_t offset_{};
    static inline PassCount     passes_{};
    static inline std::uint32_t lastCrc_{};
    static inline std::uint32_t dropped_{};
    static inline std::uint32_t consecutiveCorrupt_{};
    static inline std::uint8_t  segment_{};
    static inline State         state_{State::idle};
    static inline Result        last_{Result::unknown};
};
}   // namespace Kvasir::ImageCheck
