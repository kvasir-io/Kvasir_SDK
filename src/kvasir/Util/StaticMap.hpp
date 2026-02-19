#pragma once

#include "StaticVector.hpp"

#include <cassert>
#include <utility>

namespace Kvasir {

/// Fixed-capacity associative container with linear search.
///
/// Keys must be equality-comparable via operator==.
/// Insertion order is preserved; duplicate keys are not allowed.
template<typename Key, typename Value, size_t Capacity>
struct StaticMap {
    using key_type        = Key;
    using mapped_type     = Value;
    using value_type      = std::pair<Key, Value>;
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
            if(it->first == key) { return it; }
        }
        return end();
    }

    constexpr const_iterator find(key_type const& key) const noexcept {
        for(auto it = begin(); it != end(); ++it) {
            if(it->first == key) { return it; }
        }
        return end();
    }

    constexpr bool contains(key_type const& key) const noexcept { return find(key) != end(); }

    /// Access element by key. Asserts if key is not present.
    constexpr mapped_type& at(key_type const& key) noexcept {
        auto it = find(key);
        assert(it != end() && "key not found in StaticMap");
        return it->second;
    }

    constexpr mapped_type const& at(key_type const& key) const noexcept {
        auto it = find(key);
        assert(it != end() && "key not found in StaticMap");
        return it->second;
    }

    ///@}

    /// \name Modifiers
    ///@{

    /// Insert key-value pair.
    /// Returns {iterator, true} if inserted, {existing_iterator, false} if key already present.
    constexpr std::pair<iterator,
                        bool>
    insert(value_type const& kv) noexcept {
        auto it = find(kv.first);
        if(it != end()) { return {it, false}; }
        assert(!full() && "StaticMap is full");
        data_.push_back(kv);
        return {end() - 1, true};
    }

    constexpr std::pair<iterator,
                        bool>
    insert(value_type&& kv) noexcept {
        auto it = find(kv.first);
        if(it != end()) { return {it, false}; }
        assert(!full() && "StaticMap is full");
        data_.push_back(std::move(kv));
        return {end() - 1, true};
    }

    /// Insert if key absent, overwrite value if key present.
    /// Returns {iterator, true} if inserted, {iterator, false} if assigned.
    constexpr std::pair<iterator,
                        bool>
    insert_or_assign(key_type const&    key,
                     mapped_type const& value) noexcept {
        auto it = find(key);
        if(it != end()) {
            it->second = value;
            return {it, false};
        }
        assert(!full() && "StaticMap is full");
        data_.emplace_back(key, value);
        return {end() - 1, true};
    }

    /// Access or insert a default-constructed value for key.
    /// Requires Value to be default constructible.
    constexpr mapped_type& operator[](key_type const& key) noexcept {
        auto it = find(key);
        if(it != end()) { return it->second; }
        assert(!full() && "StaticMap is full");
        data_.emplace_back(key, mapped_type{});
        return data_.back().second;
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

    constexpr StaticMap()                                = default;
    constexpr StaticMap(StaticMap const&)                = default;
    constexpr StaticMap& operator=(StaticMap const&)     = default;
    constexpr StaticMap(StaticMap&&) noexcept            = default;
    constexpr StaticMap& operator=(StaticMap&&) noexcept = default;

    /// Construct from initializer list of {key, value} pairs.
    /// Duplicate keys are silently ignored (first one wins).
    constexpr StaticMap(std::initializer_list<value_type> il) noexcept {
        assert(il.size() <= Capacity && "initializer list size exceeds capacity");
        for(auto const& kv : il) { insert(kv); }
    }

    ///@}
};

template<typename Key,
         typename Value,
         size_t Capacity>
constexpr bool operator==(StaticMap<Key,
                                    Value,
                                    Capacity> const& a,
                          StaticMap<Key,
                                    Value,
                                    Capacity> const& b) noexcept {
    if(a.size() != b.size()) { return false; }
    for(auto const& kv : a) {
        auto it = b.find(kv.first);
        if(it == b.end() || it->second != kv.second) { return false; }
    }
    return true;
}

template<typename Key,
         typename Value,
         size_t Capacity>
constexpr bool operator!=(StaticMap<Key,
                                    Value,
                                    Capacity> const& a,
                          StaticMap<Key,
                                    Value,
                                    Capacity> const& b) noexcept {
    return !(a == b);
}

}   // namespace Kvasir
