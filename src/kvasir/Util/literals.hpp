#pragma once
#include "detail/constant_parser.hpp"

#include <limits>
#include <type_traits>

namespace Kvasir { namespace literals {
    using namespace std::literals;
    namespace detail {
        template<typename T, typename U, U Value>
        constexpr T validate_value() {
            static_assert(
              sizeof(T) <= sizeof(U) && std::is_signed<U>::value == std::is_signed<T>::value,
              "mismatched types");
            static_assert(
              U(std::numeric_limits<T>::min()) <= Value
                && Value <= U(std::numeric_limits<T>::max()),
              "integer literal overflow");
            return static_cast<T>(Value);
        }

        template<typename T, char... Digits>
        constexpr T parse_signed() {
            return validate_value<T, long long, parse<long long, Digits...>()>();
        }

        template<typename T, char... Digits>
        constexpr T parse_unsigned() {
            return validate_value<T, unsigned long long, parse<unsigned long long, Digits...>()>();
        }
    }   // namespace detail

    template<char... Digits>
    constexpr std::int8_t operator"" _i8() {
        return std::int8_t(detail::parse_signed<std::int8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int16_t operator"" _i16() {
        return std::int16_t(detail::parse_signed<std::int16_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int32_t operator"" _i32() {
        return std::int32_t(detail::parse_signed<std::int32_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::int64_t operator"" _i64() {
        return std::int64_t(detail::parse_signed<std::int64_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint8_t operator"" _u8() {
        return std::uint8_t(detail::parse_unsigned<std::uint8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::byte operator"" _b() {
        return std::byte(detail::parse_unsigned<std::uint8_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint16_t operator"" _u16() {
        return std::uint16_t(detail::parse_unsigned<std::uint16_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint32_t operator"" _u32() {
        return std::uint32_t(detail::parse_unsigned<std::uint32_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::uint64_t operator"" _u64() {
        return std::uint64_t(detail::parse_unsigned<std::uint64_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::ptrdiff_t operator"" _isize() {
        return std::ptrdiff_t(detail::parse_signed<std::ptrdiff_t, Digits...>());
    }

    template<char... Digits>
    constexpr std::size_t operator"" _usize() {
        return std::size_t(detail::parse_unsigned<std::size_t, Digits...>());
    }

    template<char... Digits>
    constexpr int operator"" _i() {
        // int is at least 16 bits
        return int(detail::parse_signed<std::int16_t, Digits...>());
    }

    template<char... Digits>
    constexpr unsigned operator"" _u() {
        // int is at least 16 bits
        return unsigned(detail::parse_unsigned<std::uint16_t, Digits...>());
    }

}}   // namespace Kvasir::literals
