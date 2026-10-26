#pragma once

#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/LinkerSymbols.hpp"
#include "kvasir/Util/PassCount.hpp"

#include <cstddef>
#include <cstdint>

/// Stack high-water mark: in a Startup list it paints the boot core's stack below SP;
/// free() counts the words still carrying the pattern (`kvasir_bench.py stack` reads it too).
namespace Kvasir {
namespace StackUsageDetail {
    inline constexpr std::uint32_t pattern = 0x5AC3'5AC3U;
    inline constexpr std::uint32_t protectorSentinel
      = 0x55AA'55AAU;   // StackProtector::sentinelVal

    // Paints [low, end) with the pattern, past the StackProtector's sentinel run at the bottom (.stackProtector).
    // Only there - RAM keeps its contents across a reset or a RAM image load, and a word that happens to equal the
    // sentinel further up, left unpainted, would end every count at it.
    [[gnu::always_inline]] inline void paint(std::uint32_t*       p,
                                             std::uint32_t const* end) {
        while(p < end && *p == protectorSentinel) { ++p; }
        for(; p < end; ++p) { *p = pattern; }
    }

    // The bytes of [low, high) still painted: the first run of pattern words, after whatever precedes it (the
    // sentinel, if there is one, is not free stack).
    [[gnu::always_inline]] inline std::size_t freeBytes(std::uint32_t const* p,
                                                        std::uint32_t const* high) {
        std::size_t words = 0;
        while(p < high && *p != pattern) { ++p; }
        while(p < high && *p == pattern) {
            ++p;
            ++words;
        }
        return words * sizeof(std::uint32_t);
    }
}   // namespace StackUsageDetail

struct StackUsage {
    static constexpr std::uint32_t pattern           = StackUsageDetail::pattern;
    static constexpr std::uint32_t protectorSentinel = StackUsageDetail::protectorSentinel;

    // headroom below SP for this frame and interrupt stacking
    static constexpr std::size_t margin = 128;

    static std::uint32_t* low() {
        // a linker symbol's address
        return reinterpret_cast<std::uint32_t*>(&_LINKER_stack_start_);
    }

    static std::uint32_t* high() { return reinterpret_cast<std::uint32_t*>(&_LINKER_stack_end_); }

    [[gnu::noinline]] static void runtimeInit() {
        auto const  sp  = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        auto* const end = reinterpret_cast<std::uint32_t*>((sp - margin) & ~std::uintptr_t{3});
        StackUsageDetail::paint(low(), end);
    }

    static std::size_t size() {
        return static_cast<std::size_t>(high() - low()) * sizeof(std::uint32_t);
    }

    static std::size_t free() { return StackUsageDetail::freeBytes(low(), high()); }
};

/// StackUsage for a running firmware: paints the stack like StackUsage and counts it again
/// in the main loop, WordsPerTurn words a turn, so a high-water mark is at hand without
/// free()'s one long scan (the whole of a RAM image's stack is a few hundred KB). List this
/// instead of StackUsage, not both. Stack: whose stack (low(), high(), size(), runtimeInit() as StackUsage's; a host
/// test hands in a buffer).
template<std::size_t WordsPerTurn = 256, typename Stack = StackUsage>
struct StackHighWater {
    [[gnu::always_inline]] static void runtimeInit() { Stack::runtimeInit(); }

    static std::size_t size() { return Stack::size(); }

    /// free() as of the last finished pass; size() before the first.
    static std::size_t free() { return passes_.none() ? size() : free_; }

    /// Finished passes; 0 only before the first (after 2^32 - 1 it goes on at 1: PassCount).
    static std::uint32_t passes() { return passes_.value(); }

    static Turn turn() {
        auto const* const high = Stack::high();
        auto const*       p    = Stack::low() + cursor_;
        for(std::size_t n = 0; n < WordsPerTurn; ++n, ++p) {
            if(p >= high || (counting_ && *p != StackUsageDetail::pattern)) {
                finish();
                return Turn::idle();
            }
            if(*p == StackUsageDetail::pattern) {
                counting_ = true;
                ++words_;
            }
        }
        cursor_ = static_cast<std::size_t>(p - Stack::low());
        return Turn::idle();
    }

    using Extends = Startup::Extend<Hook::MainLoop, &StackHighWater::turn>;

private:
    static void finish() {
        free_     = words_ * sizeof(std::uint32_t);
        cursor_   = 0;
        words_    = 0;
        counting_ = false;
        ++passes_;
    }

    inline static std::size_t cursor_{};     // words from low()
    inline static std::size_t words_{};      // pattern words of this pass
    inline static bool        counting_{};   // past the sentinel, in the painted run
    inline static std::size_t free_{};
    inline static PassCount   passes_{};
};
}   // namespace Kvasir
