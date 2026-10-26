#pragma once

#include <cstddef>
#include <iterator>
#include <ranges>

namespace Kvasir {

template<typename T, std::size_t N>
concept TypeSize = sizeof(T) == N;

template<typename R>
concept ContiguousByteRange
  = std::ranges::contiguous_range<R> && TypeSize<std::ranges::range_value_t<R>, 1>;
template<typename I>
concept ContiguousByteIterator = std::contiguous_iterator<I> && TypeSize<std::iter_value_t<I>, 1>;
}   // namespace Kvasir
