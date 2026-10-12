#pragma once

#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"

#include <cstddef>
#include <cstdint>

/// Stack high-water mark: in a Startup list it paints the boot core's stack below SP;
/// free() counts the words still carrying the pattern (`kvasir_bench.py stack` reads it too).
namespace Kvasir {
struct StackUsage {
    static constexpr std::uint32_t Pattern = 0x5AC3'5AC3U;
    static constexpr std::uint32_t ProtectorSentinel
      = 0x55AA'55AAU;   // StackProtector::sentinelVal

    // headroom below SP for this frame and interrupt stacking
    static constexpr std::size_t Margin = 128;

    static std::uint32_t* low() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a linker symbol's address
        return reinterpret_cast<std::uint32_t*>(&_LINKER_stack_start_);
    }

    static std::uint32_t* high() {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<std::uint32_t*>(&_LINKER_stack_end_);
    }

    [[gnu::noinline]] static void runtimeInit() {
        auto const sp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const end = reinterpret_cast<std::uint32_t*>((sp - Margin) & ~std::uintptr_t{3});
        // Leave StackProtector's sentinel alone: the run at the bottom (.stackProtector). Only
        // there - RAM keeps its contents across a reset or a RAM image load, and a word that
        // happens to equal the sentinel further up, left unpainted, would end every count at it.
        auto* p = low();
        while(p < end && *p == ProtectorSentinel) { ++p; }
        for(; p < end; ++p) { *p = Pattern; }
    }

    static std::size_t size() {
        return static_cast<std::size_t>(high() - low()) * sizeof(std::uint32_t);
    }

    static std::size_t free() {
        std::size_t words = 0;
        auto const* p     = low();
        // the StackProtector's sentinel, if there is one, is not free stack
        while(p < high() && *p != Pattern) { ++p; }
        while(p < high() && *p == Pattern) {
            ++p;
            ++words;
        }
        return words * sizeof(std::uint32_t);
    }
};

/// StackUsage for a running firmware: paints the stack like StackUsage and counts it again
/// in the main loop, WordsPerTurn words a turn, so a high-water mark is at hand without
/// free()'s one long scan (the whole of a RAM image's stack is a few hundred KB). List this
/// instead of StackUsage, not both.
template<std::size_t WordsPerTurn = 256>
struct StackHighWater {
    [[gnu::always_inline]] static void runtimeInit() { StackUsage::runtimeInit(); }

    static std::size_t size() { return StackUsage::size(); }

    /// free() as of the last finished pass; size() before the first.
    static std::size_t free() { return passes_ == 0 ? size() : free_; }

    /// Finished passes.
    static std::uint32_t passes() { return passes_; }

    static Turn turn() {
        auto const* const high = StackUsage::high();
        auto const*       p    = StackUsage::low() + cursor_;
        for(std::size_t n = 0; n < WordsPerTurn; ++n, ++p) {
            if(p >= high || (counting_ && *p != StackUsage::Pattern)) {
                finish();
                return Turn::idle();
            }
            if(*p == StackUsage::Pattern) {
                counting_ = true;
                ++words_;
            }
        }
        cursor_ = static_cast<std::size_t>(p - StackUsage::low());
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

    inline static std::size_t   cursor_{};     // words from low()
    inline static std::size_t   words_{};      // pattern words of this pass
    inline static bool          counting_{};   // past the sentinel, in the painted run
    inline static std::size_t   free_{};
    inline static std::uint32_t passes_{};
};
}   // namespace Kvasir
