#pragma once
// RAM set-up before any constructor: a load image copied to its run address, a range zeroed. initMemory() (.data and
// .bss, StartUp.hpp) and a chip's Startup::ExtraMemoryInit (its extra RAM regions) both use these.
//
// The arguments are linker symbols: distinct objects to the compiler, which could otherwise fold a compare of two of
// them, or take an absolute symbol's address (a size) for a real object's. Each value goes through an empty asm
// first, in argument order, so the code is a plain memcpy / memset of run-time values. T is the symbols' declared
// type: it gives the copy its alignment (a uintptr_t image is copied word-wise, __aeabi_memcpy4).
#include <cstddef>
#include <cstring>

namespace Kvasir::Startup {
namespace Detail {
    template<typename T>
    [[gnu::always_inline]] inline void hideOne(T& v) {
        asm("" : "+l"(v)::);
    }

    template<typename... T>
    [[gnu::always_inline]] inline void hideFromOptimizer(T&... v) {
        (hideOne(v), ...);
    }
}   // namespace Detail

// n bytes from load to run. With skipInPlace, nothing when load == run (a section without AT() in a RAM-only image:
// its load image is where it runs); without it such a copy is onto itself - the same bytes.
template<typename T>
[[gnu::always_inline]] inline void copyImage(T const*    load,
                                             T*          run,
                                             std::size_t n,
                                             bool        skipInPlace = false) {
    Detail::hideFromOptimizer(run, load, n);
    if(skipInPlace && static_cast<void const*>(load) == static_cast<void const*>(run)) { return; }
    std::memcpy(run, load, n);
}

// n bytes from start zeroed.
template<typename T>
[[gnu::always_inline]] inline void zeroRange(T*          start,
                                             std::size_t n) {
    Detail::hideFromOptimizer(start, n);
    std::memset(start, 0, n);
}
}   // namespace Kvasir::Startup
