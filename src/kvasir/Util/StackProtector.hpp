#pragma once

#include "uc_log/uc_log.hpp"

#include <array>
#include <cassert>
#include <cstddef>

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
};
}   // namespace Kvasir
