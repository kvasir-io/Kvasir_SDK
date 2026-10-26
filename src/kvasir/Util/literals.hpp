#pragma once
#include "detail/constant_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Kvasir { namespace literals {
    namespace detail {
        template<typename T,
                 typename U,
                 U Value>
        constexpr T validateValue() {
            static_assert(sizeof(T) <= sizeof(U) && std::is_signed_v<U> == std::is_signed_v<T>,
                          "mismatched types");
            static_assert(U(std::numeric_limits<T>::min()) <= Value
                            && Value <= U(std::numeric_limits<T>::max()),
                          "integer literal overflow");
            return static_cast<T>(Value);
        }

        template<typename T,
                 char... Digits>
        constexpr T parseSigned() {
            // a named constant first: clang then prints why it is not one (the function the parse reached)
            constexpr long long value = parse<long long, Digits...>();
            return validateValue<T, long long, value>();
        }

        template<typename T,
                 char... Digits>
        constexpr T parseUnsigned() {
            constexpr unsigned long long value = parse<unsigned long long, Digits...>();
            return validateValue<T, unsigned long long, value>();
        }
    }   // namespace detail

    template<char... Digits>
    constexpr std::int8_t operator""_i8() {
        return std::int8_t(detail::parseSigned<std::int8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int16_t operator""_i16() {
        return std::int16_t(detail::parseSigned<std::int16_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int32_t operator""_i32() {
        return std::int32_t(detail::parseSigned<std::int32_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int64_t operator""_i64() {
        return std::int64_t(detail::parseSigned<std::int64_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint8_t operator""_u8() {
        return std::uint8_t(detail::parseUnsigned<std::uint8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::byte operator""_b() {
        return std::byte(detail::parseUnsigned<std::uint8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint16_t operator""_u16() {
        return std::uint16_t(detail::parseUnsigned<std::uint16_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint32_t operator""_u32() {
        return std::uint32_t(detail::parseUnsigned<std::uint32_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint64_t operator""_u64() {
        return std::uint64_t(detail::parseUnsigned<std::uint64_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::ptrdiff_t operator""_isize() {
        return std::ptrdiff_t(detail::parseSigned<std::ptrdiff_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::size_t operator""_usize() {
        return std::size_t(detail::parseUnsigned<std::size_t, Digits...>());
    }

    // int's own range: int is 32 bits on every Kvasir target and host
    template<char... Digits>
    constexpr int operator""_i() {
        return detail::parseSigned<int, Digits...>();
    }

    template<char... Digits>
    constexpr unsigned operator""_u() {
        return detail::parseUnsigned<unsigned, Digits...>();
    }

}}   // namespace Kvasir::literals
