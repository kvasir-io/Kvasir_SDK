#pragma once

#include "kvasir/Util/StaticVector.hpp"

#include <cassert>
#include <cstddef>
#include <utility>

namespace Kvasir::Detail {

struct KeyIsValue {
    template<typename V>
    constexpr V&& operator()(V&& v) const noexcept {
        return std::forward<V>(v);
    }
};

struct KeyIsFirst {
    template<typename V>
    constexpr auto const& operator()(V const& v) const noexcept {
        return v.first;
    }
};

/// The fixed-capacity, insertion-ordered, linear-search container under StaticMap and StaticSet.
/// It declares no mapped_type: aglio and remote_fmt tell a map from a set by it.
template<typename Value, typename Key, std::size_t Capacity, typename KeyOf>
struct FlatLinear {
    using key_type        = Key;
    using value_type      = Value;
    using size_type       = std::size_t;
    using reference       = Value&;
    using const_reference = Value const&;
    using iterator        = Value*;
    using const_iterator  = Value const*;

    [[nodiscard]] constexpr size_type size() const noexcept { return data_.size(); }

    static constexpr size_type capacity() noexcept { return Capacity; }

    static constexpr size_type max_size() noexcept { return Capacity; }

    [[nodiscard]] constexpr bool empty() const noexcept { return data_.empty(); }

    [[nodiscard]] constexpr bool full() const noexcept { return data_.full(); }

    constexpr iterator begin() noexcept { return data_.begin(); }

    [[nodiscard]] constexpr const_iterator begin() const noexcept { return data_.begin(); }

    constexpr iterator end() noexcept { return data_.end(); }

    [[nodiscard]] constexpr const_iterator end() const noexcept { return data_.end(); }

    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return data_.cbegin(); }

    [[nodiscard]] constexpr const_iterator cend() const noexcept { return data_.cend(); }

    /// O(n) linear search.
    constexpr iterator find(key_type const& key) noexcept { return begin() + findIndex_(key); }

    [[nodiscard]] constexpr const_iterator find(key_type const& key) const noexcept {
        return begin() + findIndex_(key);
    }

    [[nodiscard]] constexpr bool contains(key_type const& key) const noexcept {
        return findIndex_(key) != size();
    }

    /// {iterator, true} if inserted, {existing iterator, false} if the key was there.
    constexpr std::pair<iterator,
                        bool>
    insert(value_type const& v) noexcept {
        return insertImpl_(v);
    }

    constexpr std::pair<iterator,
                        bool>
    insert(value_type&& v) noexcept {
        return insertImpl_(std::move(v));
    }

    /// Returns the iterator to the next element.
    constexpr iterator erase(const_iterator pos) noexcept { return data_.erase(pos); }

    /// Returns 1 if erased, 0 if not found.
    constexpr size_type erase(key_type const& key) noexcept {
        auto const i = findIndex_(key);
        if(i == size()) { return 0; }
        data_.erase(begin() + i);
        return 1;
    }

    constexpr void clear() noexcept { data_.clear(); }

    /// Order-insensitive: the same keys, and for a map the same values.
    friend constexpr bool operator==(FlatLinear const& a,
                                     FlatLinear const& b) noexcept {
        if(a.size() != b.size()) { return false; }
        for(auto const& v : a) {
            auto const it = b.find(KeyOf{}(v));
            if(it == b.end() || !(*it == v)) { return false; }
        }
        return true;
    }

protected:
    [[nodiscard]] constexpr size_type findIndex_(key_type const& key) const noexcept {
        size_type i = 0;
        for(; i != size() && !(KeyOf{}(data_[i]) == key); ++i) {}
        return i;
    }

    template<typename V>
    constexpr std::pair<iterator,
                        bool>
    insertImpl_(V&& v) noexcept {
        auto const i = findIndex_(KeyOf{}(v));
        if(i != size()) { return {begin() + i, false}; }
        assert(!full() && "flat container is full");
        data_.push_back(std::forward<V>(v));
        return {end() - 1, true};
    }

    // the derived map and set construct their elements in place
    constexpr StaticVector<Value,
                           Capacity>&
    storage_() noexcept {
        return data_;
    }

private:
    StaticVector<Value, Capacity> data_;
};

}   // namespace Kvasir::Detail
