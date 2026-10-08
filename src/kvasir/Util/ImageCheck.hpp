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
// What a mismatch does is injected (default Policy::Log: one crit line per pass):
//
//     template<>
//     inline constexpr auto Kvasir::ImageCheck::injectedMismatchPolicy<> = Kvasir::ImageCheck::Policy::Panic{};
//
// Policy::Panic raises Panic::Cause::imageCorrupt with the computed CRC as the detail; Policy::PanicAfter<N> logs
// each corrupt pass and raises it at the N-th in a row - unless a debugger has halting debug enabled (its software
// breakpoints are writes into the code); Policy::Call<&fn> calls fn(got, want), or fn(got, want, inARow).
//
// Backends: Software<TableSize, Reader> (every chip; on RP read through the uncached alias,
// Kvasir::Flash::UncachedRead, so the check neither evicts hot code from the XIP cache nor reads a cached copy), and
// the chips' hardware ones (RP: RpStream in chip/ImageCheck.hpp, SAM: SamDsu). A chunk is finished before step()
// returns; a flash writer on the OTHER core must stop this checker first (FlashRegion.hpp's rule).
//
// Pacing: EveryTurn<ChunkBytes> (one chunk every turn), Paced<Clock, PeriodMs, ChunkBytes> (one chunk per period).
//
// Test hook (KVASIR_IMAGE_CRC_TEST): Test::corruptNextPass(address) makes the Software backend read that one byte
// flipped, once.
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/Crc.hpp"
#include "kvasir/Util/ImageDescriptor.hpp"
#include "kvasir/Util/Panic.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

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

/// Whether a debugger has halting debug enabled: DHCSR.C_DEBUGEN (bit 0, 0xE000EDF0; Armv8-M ARM DDI0553B.y
/// D1.2.39), read on Armv8-M mainline only - the Cortex-M33 TRM (100230 C1.1.5 Table C1-4) lists DHCSR for software.
/// Everything else says no: Armv6-M software cannot read it (AnnouncedReset.hpp has the sources), and Armv7-M has
/// the register at the same address with the same bit (DDI0403E C1.6.2) but leaves "access to the DHCSR from
/// software running on the processor" IMPLEMENTATION DEFINED - a chip package whose core documents the read can
/// give PanicAfter a Debugger of its own.
///
/// Measured on the RP2350 over J-Link: software reads the bit as the probe
/// does (DHCSR 0x01100001) - except while the log printer's session starts: after it loaded and started an image
/// the bit was set at the first instruction, clear from 4..14 ms, set again from 74..99 ms and then stayed. A
/// corrupt pass in that gap is judged as if nobody were attached.
struct CoreDebug {
    /// Whether this core can ask at all; false: attached() is always false.
#if defined(__ARM_ARCH_8M_MAIN__)
    static constexpr bool readable = true;
#else
    static constexpr bool readable = false;
#endif

    [[nodiscard]] static bool attached() {
        if constexpr(readable) {
            return (*reinterpret_cast<std::uint32_t const volatile*>(0xE000'EDF0U) & 1U) != 0;
        } else {
            return false;
        }
    }
};

namespace Policy {
    struct Log {
        static void mismatch([[maybe_unused]] std::uint32_t got,
                             [[maybe_unused]] std::uint32_t want) {
#ifdef USE_UC_LOG
            UC_LOG_C("image check: CRC 0x{:08x}, the image was built with 0x{:08x}: image corrupt",
                     got,
                     want);
#endif
        }
    };

    struct Panic {
        [[noreturn]] static void mismatch(std::uint32_t got,
                                          std::uint32_t) {
            Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::imageCorrupt,
                                   static_cast<std::uint32_t>(
                                     reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),
                                   got);
        }
    };

    /// Logs every corrupt pass and panics at the Limit-th in a row (an ok pass starts the count again): one bad pass
    /// is reported before the image is given up on. Not while Debugger::attached(): a debugger's software breakpoint
    /// is a write into the code, and the session should not end in a reset - the passes are still reported.
    template<std::uint32_t Limit, typename Debugger = CoreDebug>
    struct PanicAfter {
        static_assert(Limit >= 1,
                      "PanicAfter<0> would never let a pass be corrupt: use Policy::Panic");
        static constexpr std::uint32_t limit = Limit;

        static void mismatch(std::uint32_t                  got,
                             [[maybe_unused]] std::uint32_t want,
                             std::uint32_t                  inARow) {
            bool const debugged = Debugger::attached();
#ifdef USE_UC_LOG
            UC_LOG_C(
              "image check: CRC 0x{:08x}, the image was built with 0x{:08x}: image corrupt "
              "({} of {} in a row{})",
              got,
              want,
              inARow,
              Limit,
              debugged ? std::string_view{", debugger attached: no panic"} : std::string_view{});
#endif
            if(inARow >= Limit && !debugged) {
                Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::imageCorrupt,
                                       static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
                                         __builtin_return_address(0))),
                                       got);
            }
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
}   // namespace Policy

template<typename...>
inline constexpr auto injectedMismatchPolicy = Policy::Log{};

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
    inline std::uintptr_t volatile corruptAt = 0;   // 0: nothing to corrupt

    /// The Software backend reads the byte at `address` with its lowest bit flipped, once.
    inline void corruptNextPass(std::uintptr_t address) { corruptAt = address; }
}   // namespace Test
#endif

/// Reads the image where the descriptor says it is (the reader for a RAM image). The chips offer flash readers of
/// their own (RP: the uncached alias).
struct DirectRead {
    static std::uint32_t word(std::uintptr_t address) {
        return *reinterpret_cast<std::uint32_t const volatile*>(address);
    }

    static std::uint8_t byte(std::uintptr_t address) {
        return *reinterpret_cast<std::uint8_t const volatile*>(address);
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
            b               = static_cast<std::uint8_t>(b ^ 1U);
            Test::corruptAt = 0;
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
struct Checker {
    static_assert(
      Source::configured,
      "Kvasir::ImageCheck::Checker needs the image CRC descriptor: add IMAGE_CRC to the firmware's "
      "kvasir_executable_variants (or target_configure_kvasir)");

    /// One chunk (when the pacing says so); at the end of the last segment compare, count, start over.
    static void step() {
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
        auto const  before = passes_;
        while(passes_ == before) {
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

    [[nodiscard]] static std::uint32_t passes() { return passes_; }

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

    using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Checker::step>;

private:
    enum class State : std::uint8_t { idle, running, stopped };

    static bool start() {
        auto const&         d     = Source::get();
        std::uint32_t const magic = d.magic;
        std::uint32_t const count = d.count;
        if(magic != Descriptor::Magic || d.version != Descriptor::Version || count == 0
           || count > Descriptor::MaxSegments)
        {
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
        if(passes_ == 0) {
            std::uint32_t bytes = 0;
            for(std::size_t i = 0; i != count; ++i) { bytes += d.segments[i].length; }
            std::uint32_t const crc = d.crc;
            UC_LOG_I("image check: {} segment(s), {} bytes, crc 0x{:08x}", count, bytes, crc);
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
        auto const& policy       = injectedMismatchPolicy<Dummy...>;
        using P                  = std::remove_cvref_t<decltype(policy)>;
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
            UC_LOG_I("image check: pass {} ok (crc 0x{:08x})", passes_, lastCrc_);
        }
#endif
        if(last_ == Result::corrupt) { mismatch(lastCrc_); }
    }

    static inline std::uint32_t offset_{};
    static inline std::uint32_t passes_{};
    static inline std::uint32_t lastCrc_{};
    static inline std::uint32_t dropped_{};
    static inline std::uint32_t consecutiveCorrupt_{};
    static inline std::uint8_t  segment_{};
    static inline State         state_{State::idle};
    static inline Result        last_{Result::unknown};
};
}   // namespace Kvasir::ImageCheck
