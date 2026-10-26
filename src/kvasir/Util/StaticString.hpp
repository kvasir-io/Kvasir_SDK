#pragma once

#include "StaticVector.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
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
    explicit constexpr StaticString(char const (&str)[NN]) {
        static_assert(N >= NN - 1, "Buffer too small");
        assign(std::string_view{str, NN - 1});
    }

    template<std::size_t NN>
    constexpr StaticString(StaticString<NN> const& other) {
        static_assert(N >= NN, "Buffer too small");
        assign(std::string_view{other});
    }

    explicit constexpr StaticString(std::string_view sv) { assign(sv); }

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

    [[nodiscard]] constexpr std::size_t size() const { return data_.size(); }

    // static, as StaticVector's / StaticMap's / StaticSet's: the bound is part of the type, so aglio counts the string
    // as a fixed-capacity buffer (fixed_capacity_v) and bounds its wire size (max_serialized_size_v).
    static constexpr std::size_t max_size() noexcept { return N; }

    static constexpr std::size_t capacity() noexcept { return N; }

    [[nodiscard]] constexpr bool empty() const { return data_.empty(); }

    [[nodiscard]] constexpr bool full() const { return data_.full(); }

    ///@}

    /// \name Data access
    ///@{

    [[nodiscard]] constexpr char const* data() const { return data_.data(); }

    constexpr char* data() { return data_.data(); }

    ///@}

    /// \name Iterators
    ///@{

    constexpr iterator begin() { return data_.begin(); }

    [[nodiscard]] constexpr const_iterator begin() const { return data_.begin(); }

    constexpr iterator end() { return data_.end(); }

    [[nodiscard]] constexpr const_iterator end() const { return data_.end(); }

    constexpr reverse_iterator rbegin() { return reverse_iterator{data_.end()}; }

    [[nodiscard]] constexpr const_reverse_iterator rbegin() const {
        return const_reverse_iterator{data_.end()};
    }

    constexpr reverse_iterator rend() { return reverse_iterator{data_.begin()}; }

    [[nodiscard]] constexpr const_reverse_iterator rend() const {
        return const_reverse_iterator{data_.begin()};
    }

    ///@}

    /// \name Element access
    ///@{

    constexpr char& front() { return data_.front(); }

    [[nodiscard]] constexpr char const& front() const { return data_.front(); }

    constexpr char& back() { return data_.back(); }

    [[nodiscard]] constexpr char const& back() const { return data_.back(); }

    ///@}

    /// \name Modifiers
    ///@{

    constexpr void push_back(char c) { data_.push_back(c); }

    /// Append \p sv. Asserts that it fits, like every other append.
    constexpr void put(std::string_view sv) {
        assert(N >= size() + sv.size());
        std::copy(sv.begin(), sv.end(), std::back_inserter(data_));
    }

    /// Append \p v as decimal digits, no sign, no padding. Asserts that they fit.
    constexpr void putUint(std::uint32_t v) {
        std::array<char, 10> digits{};   // 4294967295 is ten of them
        std::size_t          n = 0;
        do {
            digits[n++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while(v != 0);
        assert(N >= size() + n);
        while(n != 0) { data_.push_back(digits[--n]); }
    }

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

    [[nodiscard]] constexpr bool starts_with(std::string_view sv) const {
        if(sv.size() > size()) { return false; }
        return std::equal(sv.begin(), sv.end(), data_.begin());
    }

    [[nodiscard]] constexpr bool starts_with(char c) const { return !empty() && front() == c; }

    constexpr StaticString operator+(std::string_view sv) const {
        auto newS = *this;
        assert(N >= sv.size() + newS.size());
        std::copy(sv.begin(), sv.end(), std::back_inserter(newS));
        return newS;
    }

    ///@}

    /// \name Comparison operators
    ///@{

    /// Two hidden friends; C++20's rewritten candidates add either operand order. The string_view one
    /// covers string literals too. A StaticString on both sides needs its own (exact) overload: through
    /// the string_view one alone, `a == b` is ambiguous between the call and its reversed form.
    friend constexpr bool operator==(StaticString const& a,
                                     std::string_view    b) noexcept {
        return std::string_view{a} == b;
    }

    template<std::size_t M>
    friend constexpr bool operator==(StaticString const&    a,
                                     StaticString<M> const& b) noexcept {
        return std::string_view{a} == std::string_view{b};
    }

    ///@}

private:
    constexpr void assign(std::string_view sv) {
        assert(N >= sv.size());
        data_.clear();
        std::copy(sv.begin(), sv.end(), std::back_inserter(data_));
    }
};

}   // namespace Kvasir
