#pragma once

#include <iterator>
#include <ranges>

namespace Kvasir {

template<typename T, std::size_t N>
concept type_size = requires { requires sizeof(T) == N; };

template<typename R>
concept contiguous_byte_range
  = std::ranges::contiguous_range<R> && type_size<std::ranges::range_value_t<R>, 1>;
template<typename I>
concept contiguous_byte_iterator
  = std::contiguous_iterator<I> && type_size<std::iter_value_t<I>, 1>;
}   // namespace Kvasir
