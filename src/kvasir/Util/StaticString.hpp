#pragma once

#include "StaticVector.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string_view>

namespace Kvasir {

template<std::size_t N>
struct StaticString {
    using value_type = char;

    using iterator       = char*;
    using const_iterator = char const*;

    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

private:
    StaticVector<char, N> data_;

public:
    /// \name Construct/copy/move/destroy
    ///@{

    constexpr StaticString() = default;

    template<std::size_t NN>
    constexpr StaticString(char const (&str)[NN]) {
        static_assert(N >= NN - 1, "Buffer too small");
        assign(std::string_view{str, NN - 1});
    }

    template<std::size_t NN>
    constexpr StaticString(StaticString<NN> const& other) {
        static_assert(N >= NN, "Buffer too small");
        assign(std::string_view{other});
    }

    constexpr StaticString(std::string_view sv) { assign(sv); }

    template<std::size_t NN>
    constexpr StaticString& operator=(char const (&str)[NN]) {
        static_assert(N >= NN - 1, "Buffer too small");
        assign(std::string_view{str, NN - 1});
        return *this;
    }

    template<std::size_t NN>
    constexpr StaticString& operator=(StaticString<NN> const& other) {
        static_assert(N >= NN, "Buffer too small");
        assign(std::string_view{other});
        return *this;
    }

    constexpr StaticString& operator=(std::string_view sv) {
        assign(sv);
        return *this;
    }

    ///@}

    constexpr operator std::string_view() const {
        return std::string_view{data_.data(), data_.size()};
    }

    /// \name Size / capacity
    ///@{

    constexpr std::size_t size() const { return data_.size(); }

    constexpr std::size_t max_size() const { return N; }

    constexpr std::size_t capacity() const { return N; }

    constexpr bool empty() const { return data_.empty(); }

    constexpr bool full() const { return data_.full(); }

    ///@}

    /// \name Data access
    ///@{

    constexpr char const* data() const { return data_.data(); }

    constexpr char* data() { return data_.data(); }

    ///@}

    /// \name Iterators
    ///@{

    constexpr iterator begin() { return data_.begin(); }

    constexpr const_iterator begin() const { return data_.begin(); }

    constexpr iterator end() { return data_.end(); }

    constexpr const_iterator end() const { return data_.end(); }

    constexpr reverse_iterator rbegin() { return reverse_iterator{data_.end()}; }

    constexpr const_reverse_iterator rbegin() const { return const_reverse_iterator{data_.end()}; }

    constexpr reverse_iterator rend() { return reverse_iterator{data_.begin()}; }

    constexpr const_reverse_iterator rend() const { return const_reverse_iterator{data_.begin()}; }

    ///@}

    /// \name Element access
    ///@{

    constexpr char& front() { return data_.front(); }

    constexpr char const& front() const { return data_.front(); }

    constexpr char& back() { return data_.back(); }

    constexpr char const& back() const { return data_.back(); }

    ///@}

    /// \name Modifiers
    ///@{

    constexpr void push_back(char c) { data_.push_back(c); }

    constexpr void clear() { data_.clear(); }

    /// Resize to \p sz characters. Truncates if sz < size(), pads with '\\0' if sz > size().
    constexpr void resize(std::size_t sz) {
        assert(N >= sz);
        data_.resize(sz);
    }

    /// Erase characters in [first, last). Returns iterator to first element after erased range.
    constexpr iterator erase(iterator first,
                             iterator last) {
        return data_.erase(first, last);
    }

    ///@}

    /// \name String operations
    ///@{

    constexpr bool starts_with(std::string_view sv) const {
        if(sv.size() > size()) { return false; }
        return std::equal(sv.begin(), sv.end(), data_.begin());
    }

    constexpr bool starts_with(char c) const { return !empty() && front() == c; }

    StaticString operator+(std::string_view sv) const {
        auto newS = *this;
        assert(N >= sv.size() + newS.size());
        for(char c : sv) { newS.push_back(c); }
        return newS;
    }

    ///@}

    /// \name Comparison operators
    ///@{

    constexpr bool operator==(std::string_view sv) const { return sv == std::string_view{*this}; }

    constexpr bool operator==(StaticString const& other) const {
        return std::string_view{other} == std::string_view{*this};
    }

    template<std::size_t NN>
    constexpr bool operator==(StaticString<NN> const& other) const {
        return std::string_view{other} == std::string_view{*this};
    }

    template<std::size_t NN>
    constexpr bool operator==(char const (&str)[NN]) const {
        return std::string_view{str, NN - 1} == std::string_view{*this};
    }

    ///@}

private:
    constexpr void assign(std::string_view sv) {
        assert(N >= sv.size());
        data_.clear();
        for(char c : sv) { data_.push_back(c); }
    }
};

template<char... chars>
constexpr StaticString<sizeof...(chars)> operator""_ss() {
    StaticString<sizeof...(chars)> ret;
    if constexpr(sizeof...(chars) > 0) { (ret.push_back(chars), ...); }
    return ret;
}

}   // namespace Kvasir
