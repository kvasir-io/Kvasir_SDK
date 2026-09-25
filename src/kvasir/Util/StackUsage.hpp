#pragma once

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
        // leave StackProtector's sentinel alone
        for(auto* p = low(); p < end; ++p) {
            if(*p != ProtectorSentinel) { *p = Pattern; }
        }
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
}   // namespace Kvasir
