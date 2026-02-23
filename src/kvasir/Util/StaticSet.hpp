#pragma once

#include "StaticVector.hpp"

#include <cassert>
#include <utility>

namespace Kvasir {

/// Fixed-capacity set with linear search.
///
/// Keys must be equality-comparable via operator==.
/// Insertion order is preserved; duplicate elements are not allowed.
template<typename Key, size_t Capacity>
struct StaticSet {
    using key_type        = Key;
    using value_type      = Key;
    using size_type       = size_t;
    using reference       = value_type&;
    using const_reference = value_type const&;
    using iterator        = value_type*;
    using const_iterator  = value_type const*;

private:
    StaticVector<value_type, Capacity> data_;

public:
    /// \name Size / capacity
    ///@{

    constexpr size_type size() const noexcept { return data_.size(); }

    static constexpr size_type capacity() noexcept { return Capacity; }

    static constexpr size_type max_size() noexcept { return Capacity; }

    constexpr bool empty() const noexcept { return data_.empty(); }

    constexpr bool full() const noexcept { return data_.full(); }

    ///@}

    /// \name Iterators
    ///@{

    constexpr iterator begin() noexcept { return data_.begin(); }

    constexpr const_iterator begin() const noexcept { return data_.begin(); }

    constexpr iterator end() noexcept { return data_.end(); }

    constexpr const_iterator end() const noexcept { return data_.end(); }

    constexpr const_iterator cbegin() const noexcept { return data_.cbegin(); }

    constexpr const_iterator cend() const noexcept { return data_.cend(); }

    ///@}

    /// \name Lookup (O(n) linear search)
    ///@{

    constexpr iterator find(key_type const& key) noexcept {
        for(auto it = begin(); it != end(); ++it) {
            if(*it == key) { return it; }
        }
        return end();
    }

    constexpr const_iterator find(key_type const& key) const noexcept {
        for(auto it = begin(); it != end(); ++it) {
            if(*it == key) { return it; }
        }
        return end();
    }

    constexpr bool contains(key_type const& key) const noexcept { return find(key) != end(); }

    ///@}

    /// \name Modifiers
    ///@{

    /// Insert element.
    /// Returns {iterator, true} if inserted, {existing_iterator, false} if already present.
    constexpr std::pair<iterator,
                        bool>
    insert(value_type const& key) noexcept {
        auto it = find(key);
        if(it != end()) { return {it, false}; }
        assert(!full() && "StaticSet is full");
        data_.push_back(key);
        return {end() - 1, true};
    }

    constexpr std::pair<iterator,
                        bool>
    insert(value_type&& key) noexcept {
        auto it = find(key);
        if(it != end()) { return {it, false}; }
        assert(!full() && "StaticSet is full");
        data_.push_back(std::move(key));
        return {end() - 1, true};
    }

    /// Erase element at iterator position. Returns iterator to next element.
    constexpr iterator erase(const_iterator pos) noexcept { return data_.erase(pos); }

    /// Erase element by key. Returns 1 if erased, 0 if not found.
    constexpr size_type erase(key_type const& key) noexcept {
        auto it = find(key);
        if(it == end()) { return 0; }
        data_.erase(it);
        return 1;
    }

    constexpr void clear() noexcept { data_.clear(); }

    ///@}

    /// \name Construct/copy/move/destroy
    ///@{

    constexpr StaticSet()                                = default;
    constexpr StaticSet(StaticSet const&)                = default;
    constexpr StaticSet& operator=(StaticSet const&)     = default;
    constexpr StaticSet(StaticSet&&) noexcept            = default;
    constexpr StaticSet& operator=(StaticSet&&) noexcept = default;

    /// Construct from initializer list of keys.
    /// Duplicate keys are silently ignored (first one wins).
    constexpr StaticSet(std::initializer_list<value_type> il) noexcept {
        assert(il.size() <= Capacity && "initializer list size exceeds capacity");
        for(auto const& key : il) { insert(key); }
    }

    ///@}
};

template<typename Key,
         size_t Capacity>
constexpr bool operator==(StaticSet<Key,
                                    Capacity> const& a,
                          StaticSet<Key,
                                    Capacity> const& b) noexcept {
    if(a.size() != b.size()) { return false; }
    for(auto const& key : a) {
        if(!b.contains(key)) { return false; }
    }
    return true;
}

template<typename Key,
         size_t Capacity>
constexpr bool operator!=(StaticSet<Key,
                                    Capacity> const& a,
                          StaticSet<Key,
                                    Capacity> const& b) noexcept {
    return !(a == b);
}

}   // namespace Kvasir
