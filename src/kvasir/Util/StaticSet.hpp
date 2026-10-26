#pragma once

#include "kvasir/Util/detail/FlatLinear.hpp"

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace Kvasir {

/// Fixed-capacity set with linear search.
///
/// Keys must be equality-comparable via operator==.
/// Insertion order is preserved; duplicate elements are not allowed.
template<typename Key, std::size_t Capacity>
struct StaticSet : Detail::FlatLinear<Key, Key, Capacity, Detail::KeyIsValue> {
    constexpr StaticSet() = default;

    /// Duplicate keys are ignored (the first one wins).
    constexpr StaticSet(std::initializer_list<Key> il) noexcept {
        assert(il.size() <= Capacity && "initializer list size exceeds capacity");
        for(auto const& key : il) { this->insert(key); }
    }

    /// Insert without the duplicate search (O(1)).
    /// Preconditions: the key is not present and the set is not full; the caller ensures both.
    template<typename K2>
    constexpr void insertUnchecked(K2&& key) noexcept {
        assert(!this->full() && "StaticSet is full");
        this->storage_().emplace_back(std::forward<K2>(key));
    }
};

}   // namespace Kvasir
