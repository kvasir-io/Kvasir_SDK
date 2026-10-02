#pragma once
// Averages over a stream of samples. Nothing here reads a clock: one push per sample, and a
// caller that steps on a time grid does that itself (Kvasir::Every's dueCount).
//
//   MovingAverage<T, N>  mean of the last N; integral samples summed exactly in 64 bits
//   Ema<T, W>            v += (x - v) * alpha; works on quantities
//   EmaDiv<T>            v += (x - v) / divisor, integer only, truncating
//
// An empty filter is empty, not zero: the values are std::optional.

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>

namespace Kvasir::Control {

/// Mean of the last `N` samples.
///
/// Until N have arrived the mean is over the ones that have: an empty slot is not a zero,
/// so the first readings after power-up are not dragged towards it. Integral samples are
/// summed in 64 bits, exactly.
template<typename T, std::size_t N>
class MovingAverage {
public:
    constexpr void push(T v) {
        buffer_[next_] = v;
        next_          = (next_ + 1) % N;
        if(count_ < N) { ++count_; }
    }

    /// Every slot set to `v`: as if it had been the input for N samples.
    constexpr void fill(T v) {
        buffer_.fill(v);
        count_ = N;
        next_  = 0;
    }

    constexpr void reset() {
        count_ = 0;
        next_  = 0;
    }

    [[nodiscard]] constexpr std::size_t count() const { return count_; }

    [[nodiscard]] constexpr bool full() const { return count_ == N; }

    /// The mean in `R`; nothing before the first sample.
    template<typename R = T>
    [[nodiscard]] constexpr std::optional<R> average() const {
        if(count_ == 0) { return std::nullopt; }
        // Before the window is full the samples are the first `count_` slots.
        auto const sum
          = std::ranges::fold_left(std::span<T const>{buffer_}.first(count_), Sum{}, std::plus<>{});
        return static_cast<R>(sum) / static_cast<R>(count_);
    }

private:
    using Sum = std::conditional_t<std::integral<T>, std::int64_t, T>;

    std::array<T, N> buffer_{};
    std::size_t      next_{};
    std::size_t      count_{};
};

/// Exponential moving average: the first sample is taken as it is, every later one moves the
/// value `alpha` of the way towards it (0 < alpha <= 1). T may be a quantity (mp-units): only
/// its - and + and a multiplication by W are used.
template<typename T, typename W = float>
class Ema {
public:
    constexpr explicit Ema(W alpha) : alpha_{alpha} {}

    constexpr void push(T x) { v_ = v_ ? *v_ + (x - *v_) * alpha_ : x; }

    constexpr void reset() { v_.reset(); }

    constexpr void reset(T x) { v_ = x; }

    [[nodiscard]] constexpr std::optional<T> value() const { return v_; }

private:
    W                alpha_;
    std::optional<T> v_{};
};

/// v += (x - v) / divisor in integers, truncating toward zero: the
/// value stops up to divisor - 1 counts short of a constant input (a dead band). The first
/// sample is taken as it is.
template<std::integral T>
class EmaDiv {
public:
    constexpr explicit EmaDiv(T divisor) : divisor_{divisor} {}

    constexpr void push(T x) { v_ = v_ ? static_cast<T>(*v_ + (x - *v_) / divisor_) : x; }

    constexpr void reset() { v_.reset(); }

    constexpr void reset(T x) { v_ = x; }

    [[nodiscard]] constexpr std::optional<T> value() const { return v_; }

private:
    T                divisor_;
    std::optional<T> v_{};
};

}   // namespace Kvasir::Control
