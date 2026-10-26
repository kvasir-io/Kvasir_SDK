#pragma once
// The outcome of a call that can fail for a reason the caller may act on: the world misbehaved, the code is fine.
// `std::optional` stays "maybe no data", never "it failed"; a bool is fine where the object has an accessor that gives
// the reason (a store's retry().last).
#include <expected>

namespace Kvasir {
template<typename T, typename E>
using Result = std::expected<T, E>;

/// Drops a [[nodiscard]] result on purpose: the caller has another way to the outcome (a store's pending()/retry(), a
/// later turn()).
template<typename T,
         typename E>
constexpr void discard(std::expected<T,
                                     E> const&) noexcept {}
}   // namespace Kvasir
