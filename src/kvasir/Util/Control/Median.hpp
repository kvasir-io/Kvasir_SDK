#pragma once
// Medians: of a span (partly sorts it), and of the last N samples.

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace Kvasir::Control {

/// The median of `values`, which this reorders (nth_element). Upper median for an even count.
template<typename T>
[[nodiscard]] constexpr T median(std::span<T> values) {
    auto const mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::ranges::nth_element(values, mid);
    return *mid;
}

/// The median of the last N samples (N odd); empty until N have arrived.
template<typename T, std::size_t N>
class MedianOf {
    static_assert(N % 2 == 1,
                  "MedianOf: N must be odd");

public:
    constexpr void push(T v) {
        ring_[next_] = v;
        next_        = (next_ + 1) % N;
        if(count_ < N) { ++count_; }
    }

    constexpr void reset() {
        count_ = 0;
        next_  = 0;
    }

    [[nodiscard]] constexpr std::optional<T> value() const {
        if(count_ < N) { return std::nullopt; }
        auto copy = ring_;
        return median(std::span<T>{copy});
    }

private:
    std::array<T, N> ring_{};
    std::size_t      next_{};
    std::size_t      count_{};
};

}   // namespace Kvasir::Control
