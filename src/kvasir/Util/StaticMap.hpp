#pragma once

#include "kvasir/Util/detail/FlatLinear.hpp"

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace Kvasir {

/// Fixed-capacity associative container with linear search.
///
/// Keys must be equality-comparable via operator==.
/// Insertion order is preserved; duplicate keys are not allowed.
/// The element is a std::pair: aglio's remove_pair_const and remote_fmt's map mapping take it apart as one.
template<typename Key, typename Value, std::size_t Capacity>
struct StaticMap : Detail::FlatLinear<std::pair<Key, Value>, Key, Capacity, Detail::KeyIsFirst> {
    using mapped_type = Value;

    constexpr StaticMap() = default;

    /// Duplicate keys are ignored (the first one wins).
    constexpr StaticMap(std::initializer_list<std::pair<Key,
                                                        Value>> il) noexcept {
        assert(il.size() <= Capacity && "initializer list size exceeds capacity");
        for(auto const& kv : il) { this->insert(kv); }
    }

    /// Asserts that the key is present.
    constexpr mapped_type& at(Key const& key) noexcept {
        auto it = this->find(key);
        assert(it != this->end() && "key not found in StaticMap");
        return it->second;
    }

    [[nodiscard]] constexpr mapped_type const& at(Key const& key) const noexcept {
        auto it = this->find(key);
        assert(it != this->end() && "key not found in StaticMap");
        return it->second;
    }

    /// {iterator, true} if inserted, {iterator, false} if the value was assigned.
    constexpr std::pair<typename StaticMap::iterator,
                        bool>
    insert_or_assign(Key const&         key,
                     mapped_type const& value) noexcept {
        auto it = this->find(key);
        if(it != this->end()) {
            it->second = value;
            return {it, false};
        }
        assert(!this->full() && "StaticMap is full");
        this->storage_().emplace_back(key, value);
        return {this->end() - 1, true};
    }

    /// The value of \p key, inserted value-initialised if absent.
    constexpr mapped_type& operator[](Key const& key) noexcept {
        auto it = this->find(key);
        if(it != this->end()) { return it->second; }
        assert(!this->full() && "StaticMap is full");
        this->storage_().emplace_back(key, mapped_type{});
        return this->storage_().back().second;
    }

    /// Insert without the duplicate search (O(1)).
    /// Preconditions: the key is not present and the map is not full; the caller ensures both.
    template<typename K2,
             typename V2>
    constexpr void insertUnchecked(K2&& key,
                                   V2&& value) noexcept {
        assert(!this->full() && "StaticMap is full");
        this->storage_().emplace_back(std::forward<K2>(key), std::forward<V2>(value));
    }
};

}   // namespace Kvasir
