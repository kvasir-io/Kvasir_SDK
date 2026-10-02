#pragma once
// Piecewise-linear lookup tables, clamped outside their range.
//
//   inline constexpr std::array<Kvasir::Control::Point<int, int>, 3> Ntc{{{-400, 3900}, {0, 3100},
//                                                                        {1000, 380}}};
//   Kvasir::Control::interpolate<Ntc>(x);   // x must strictly increase, checked at compile time
//
//   inline constexpr Kvasir::Control::UniformTable<float, 6> T{0.0f, 10.0f, {100, 179, 157, 113, 102, 100}};
//   T(x);                                   // an even grid: an index, no search
//
// Integral values are interpolated in 64 bits, truncating toward zero.

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace Kvasir::Control {

template<typename X, typename Y>
struct Point {
    X x;
    Y y;
};

template<typename X,
         typename Y,
         std::size_t N>
[[nodiscard]] constexpr bool strictlyIncreasing(std::array<Point<X,
                                                                 Y>,
                                                           N> const& table) {
    for(std::size_t i = 1; i < N; ++i) {
        if(!(table[i - 1].x < table[i].x)) { return false; }
    }
    return true;
}

namespace detail {
    template<typename Y,
             typename X>
    constexpr Y lerp(X x,
                     X x0,
                     X x1,
                     Y y0,
                     Y y1) {
        if constexpr(std::integral<X> && std::integral<Y>) {
            auto const num = static_cast<std::int64_t>(x - x0)
                           * (static_cast<std::int64_t>(y1) - static_cast<std::int64_t>(y0));
            return static_cast<Y>(static_cast<std::int64_t>(y0)
                                  + num / static_cast<std::int64_t>(x1 - x0));
        } else {
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
        }
    }
}   // namespace detail

/// y at x from a table with strictly increasing x; the first/last y outside the table.
template<auto const& Table>
[[nodiscard]] constexpr auto interpolate(decltype(Table[0].x) x) {
    static_assert(Table.size() >= 2, "Kvasir::Control::interpolate: a table needs two points");
    static_assert(strictlyIncreasing(Table),
                  "Kvasir::Control::interpolate: the table's x values must strictly increase");
    using Y = decltype(Table[0].y);
    if(!(Table.front().x < x)) { return Table.front().y; }
    if(!(x < Table.back().x)) { return Table.back().y; }
    std::size_t lo = 0;
    std::size_t hi = Table.size() - 1;   // Table[lo].x < x < Table[hi].x
    while(hi - lo > 1) {
        auto const mid = lo + (hi - lo) / 2;
        if(Table[mid].x <= x) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return detail::lerp<Y>(x, Table[lo].x, Table[hi].x, Table[lo].y, Table[hi].y);
}

/// N values on an even grid from x0 in steps of `step`; y[0] below x0, y[N-1] from the last
/// knot on. Floating point: written in the order y[i] + (y[i+1] - y[i]) * into / step.
template<typename Y, std::size_t N>
struct UniformTable {
    static_assert(N >= 2,
                  "UniformTable needs two points");

    Y                x0;
    Y                step;
    std::array<Y, N> y;

    [[nodiscard]] constexpr Y operator()(Y x) const {
        if(!(x0 < x)) { return y.front(); }
        auto const i = static_cast<std::size_t>((x - x0) / step);
        if(i >= N - 1) { return y.back(); }
        Y const into = (x - x0) - (static_cast<Y>(i) * step);
        return y[i] + ((y[i + 1] - y[i]) * into / step);
    }
};

}   // namespace Kvasir::Control
