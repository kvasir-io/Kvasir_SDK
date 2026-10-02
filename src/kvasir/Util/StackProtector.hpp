#pragma once

#include "kvasir/StartUp/Hooks.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir {

template<std::size_t Size = 32>
struct StackProtector {
    static constexpr std::uint32_t sentinelVal{0x55AA55AA};

    static_assert(Size % sizeof(sentinelVal) == 0,
                  "size not valid");

    using Sentinel_t = std::array<std::decay_t<decltype(sentinelVal)>, Size / sizeof(sentinelVal)>;

    [[gnu::section(".stackProtector")]] static inline Sentinel_t sentinel{};

    [[gnu::always_inline]] static void runtimeInit() {
        std::fill(sentinel.begin(), sentinel.end(), sentinelVal);
    }

    static void handler() {
        if(std::any_of(sentinel.begin(), sentinel.end(), [](auto v) { return v != sentinelVal; })) {
            assert(false);
        }
    }

    // A turn in the main loop: Startup::run<Hook::MainLoop>() calls handler() (not by hand too).
    using Extends = Startup::Extend<Hook::MainLoop, &StackProtector::handler>;
};
}   // namespace Kvasir
