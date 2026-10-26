#pragma once

#include <cstdint>
#include <limits>

namespace Kvasir {
/// A count of finished passes (an image check, a stack scan) in which 0 means only "no pass finished yet": after
/// 2^32 - 1 it goes on at 1, never back to 0, so a reader that takes 0 for "not counted" (StackHighWater::free())
/// is not fooled once every 2^32 passes. A host reports the number as it is.
class PassCount {
    std::uint32_t n_{};

public:
    constexpr PassCount() = default;

    constexpr explicit PassCount(std::uint32_t n) : n_{n} {}

    [[nodiscard]] constexpr std::uint32_t value() const { return n_; }

    /// No pass finished yet.
    [[nodiscard]] constexpr bool none() const { return n_ == 0; }

    constexpr PassCount& operator++() {
        n_ = n_ == std::numeric_limits<std::uint32_t>::max() ? 1U : n_ + 1U;
        return *this;
    }
};
}   // namespace Kvasir
