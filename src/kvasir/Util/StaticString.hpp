#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace Kvasir {
template<std::size_t N>
struct StaticString {
    std::array<char, N> buff{};
    std::size_t         size_{};

    using value_type = char;

    using iterator       = char*;
    using const_iterator = char const*;

    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    StaticString() = default;

    constexpr StaticString(std::string_view sv) { *this = sv; }

    constexpr StaticString& operator=(std::string_view sv) {
        assert(N >= sv.size());
        std::copy(sv.begin(), sv.end(), buff.begin());
        size_ = sv.size();
        return *this;
    }

    constexpr operator std::string_view() const { return std::string_view{buff.data(), size_}; }

    constexpr bool starts_with(std::string_view sv) const {
        if(sv.size() > size()) {
            return false;
        }
        return std::equal(sv.begin(), sv.end(), buff.begin());
    }

    StaticString operator+(std::string_view sv) const {
        auto newS = *this;
        assert(N >= sv.size() + newS.size());
        std::copy(sv.begin(), sv.end(), newS.begin() + newS.size());
        newS.size_ = sv.size() + newS.size();
        return newS;
    }

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

    constexpr iterator erase(iterator first, iterator last) {
        std::size_t const charsToRemove = static_cast<std::size_t>(std::distance(first, last));
        std::size_t const newSize       = size() - charsToRemove;
        std::copy(last, end(), first);
        resize(newSize);

        return first + 1;
    }

    constexpr bool starts_with(char c) const { return !empty() && front() == c; }

    constexpr auto        data() const { return buff.data(); }
    constexpr auto        data() { return buff.data(); }
    constexpr std::size_t size() const { return size_; }
    constexpr std::size_t max_size() const { return N; }
    constexpr void        resize(std::size_t size) {
        assert(N >= size);
        size_ = size;
    }

    constexpr void push_back(char c) {
        assert(N >= size_);
        buff[size_] = c;
        ++size_;
    }
    constexpr bool        empty() const { return size_ == 0; }
    constexpr void        clear() { size_ = 0; }
    constexpr std::size_t capacity() const { return N; }

    constexpr auto begin() const { return buff.begin(); }
    constexpr auto end() const {
        return std::next(buff.begin(), static_cast<std::make_signed_t<decltype(size_)>>(size_));
    }

    constexpr auto begin() { return buff.begin(); }
    constexpr auto end() {
        return std::next(buff.begin(), static_cast<std::make_signed_t<decltype(size_)>>(size_));
    }

    constexpr auto rbegin() const { return const_reverse_iterator{end()}; }
    constexpr auto rend() const { return const_reverse_iterator{begin()}; }

    constexpr auto rbegin() { return reverse_iterator{end()}; }
    constexpr auto rend() { return reverse_iterator{begin()}; }

    constexpr char&       front() { return buff[0]; }
    constexpr char const& front() const { return buff[0]; }

    constexpr char&       back() { return buff[size_ - 1]; }
    constexpr char const& back() const { return buff[size_ - 1]; }
};

template<char... chars>
constexpr StaticString<sizeof...(chars)> operator"" _ss() {
    StaticString<sizeof...(chars)> ret;
    std::array                     s{chars...};
    std::copy(s.begin(), s.end(), ret.buff.begin());
    s.resize(ret.size());
    return ret;
};

}   // namespace Kvasir
