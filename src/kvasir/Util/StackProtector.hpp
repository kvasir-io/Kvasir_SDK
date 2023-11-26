#pragma once

#include "uc_log/uc_log.hpp"

#include <array>
#include <cassert>
#include <cstddef>

namespace Kvasir {

template<std::size_t Size = 32>
struct StackProtector {
    static constexpr std::uint32_t sentinalVal{0x55AA55AA};

    static_assert(Size % sizeof(sentinalVal) == 0, "size not valid");

    using Sentinal_t = std::array<std::decay_t<decltype(sentinalVal)>, Size / sizeof(sentinalVal)>;

    [[gnu::section(".stackProtector")]] static inline Sentinal_t sentinal{};
    [[gnu::always_inline]] static void                           runtimeInit() {
        std::fill(sentinal.begin(), sentinal.end(), sentinalVal);
    }

    static void handler() {
        if(std::any_of(sentinal.begin(), sentinal.end(), [](auto v) { return v != sentinalVal; })) {
            assert(false);
        }
    }
};
}   // namespace Kvasir
