#pragma once
// Compile errors that name what they are about: the register and field, the peripheral, the resource.
//
//     static constexpr bool fits = literalFits(Mask, Value);
//     KVASIR_STATIC_ASSERT(fits, (Register::Diagnostic::LiteralTooWide<Address, Mask, Value>),
//                          "literal does not fit the field");        // the text with KVASIR_NAMED_DIAGNOSTICS=0
//
// A message is a C++26 user-generated static_assert message (P2741: an object with constant size()
// and data()) built by a generator - a class template whose consteval operator() returns a Text.
//
// THE RULE: build a message only when the assert fails. clang evaluates a user-generated message even
// for a PASSING assert (gcc does not), at roughly 30x the compile cost of a string literal.
// KVASIR_STATIC_ASSERT therefore asserts Check<cond, Gen>::value: a passing check is a class with one
// bool, only Check<false, Gen> holds the static_assert whose message is Gen{}(). lazy<Ok, Gen>() is
// for a message that must be the caller's own static_assert's (it costs more per passing site).
//
// KVASIR_NAMED_DIAGNOSTICS=0 gives every converted site its old literal text, word for word.
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef KVASIR_NAMED_DIAGNOSTICS
    #if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
        #define KVASIR_NAMED_DIAGNOSTICS 1
    #else
        #define KVASIR_NAMED_DIAGNOSTICS 0
    #endif
#endif

namespace Kvasir::Diagnostic {

/// A compile-time message: what static_assert prints. Longer text is cut at the capacity.
struct Text {
    std::array<char, 768> chars{};
    std::size_t           length{};

    [[nodiscard]] constexpr std::size_t size() const { return length; }

    [[nodiscard]] constexpr char const* data() const { return chars.data(); }

    [[nodiscard]] constexpr std::string_view view() const { return {chars.data(), length}; }

    constexpr Text& operator<<(std::string_view s) {
        for(char const c : s) {
            if(length < chars.size()) { chars[length++] = c; }
        }
        return *this;
    }

    constexpr Text& dec(std::uint64_t v) {
        std::array<char, 24> b{};
        auto const           r = std::to_chars(b.data(), b.data() + b.size(), v);
        return *this << std::string_view{b.data(), static_cast<std::size_t>(r.ptr - b.data())};
    }

    constexpr Text& sdec(std::int64_t v) {
        std::array<char, 24> b{};
        auto const           r = std::to_chars(b.data(), b.data() + b.size(), v);
        return *this << std::string_view{b.data(), static_cast<std::size_t>(r.ptr - b.data())};
    }

    /// "0x4010000C": eight hex digits, upper case, as a datasheet writes an address
    constexpr Text& hex(std::uint32_t v) {
        constexpr std::string_view digits = "0123456789ABCDEF";
        *this << "0x";
        for(int i = 7; i >= 0; --i) { *this << digits.substr((v >> (4 * i)) & 0xFU, 1); }
        return *this;
    }

    /// A mask as bits: "15:0", "7", or the hex value when it is not one contiguous run
    constexpr Text& bits(std::uint32_t mask) {
        if(mask == 0) { return *this << "none"; }
        unsigned lo = 0;
        while(((mask >> lo) & 1U) == 0) { ++lo; }
        unsigned hi = lo;
        while(hi < 31 && ((mask >> (hi + 1)) & 1U) != 0) { ++hi; }
        std::uint32_t const run = hi == 31 ? ~0U << lo : ((1U << (hi + 1)) - 1U) & (~0U << lo);
        if(run != mask) { return hex(mask); }
        dec(hi);
        if(hi != lo) {
            *this << ":";
            dec(lo);
        }
        return *this;
    }
};

/// The message of an assert that holds: nothing to build.
struct Empty {
    [[nodiscard]] constexpr std::size_t size() const { return 0; }

    [[nodiscard]] constexpr char const* data() const { return ""; }
};

/// The message of KVASIR_STATIC_ASSERT: Gen{}() only when the assert fails (see THE RULE above).
template<bool Ok,
         typename Gen>
consteval auto lazy() {
    if constexpr(Ok) {
        return Empty{};
    } else {
        return Gen{}();
    }
}

/// "Kvasir::I2C::Master<HW::I2c0Config, 3>": T's name as the compiler spells it, from __PRETTY_FUNCTION__
/// (clang "... [T = X]", gcc "... [with T = X; ...]").
template<typename T>
consteval std::string_view typeName() {
    std::string_view const f = __PRETTY_FUNCTION__;
    auto const             b = f.find("T = ") + 4;
#if defined(__clang__)
    auto const e = f.rfind(']');
#else
    auto const e = f.find_first_of(";]", b);
#endif
    return f.substr(b, e - b);
}

/// A type name with every template argument list longer than `limit` characters shown as "<...>", the outer names
/// kept: "Kvasir::UART::UartStream<...>", "HW::LedPinConfig". What uc_log's printer shows for a long signature.
constexpr void appendAbbreviated(Text&            t,
                                 std::string_view name,
                                 std::size_t      limit = 24) {
    std::size_t i = 0;
    while(i < name.size()) {
        if(name[i] != '<') {
            t << name.substr(i, 1);
            ++i;
            continue;
        }
        std::size_t depth = 0;
        std::size_t j     = i;
        for(; j < name.size(); ++j) {
            if(name[j] == '<') { ++depth; }
            if(name[j] == '>' && --depth == 0) { break; }
        }
        if(j - i - 1 > limit) {
            t << "<...>";
        } else {
            t << name.substr(i, j - i + 1);
        }
        i = j + 1;
    }
}

template<typename T>
constexpr void appendType(Text& t) {
    appendAbbreviated(t, typeName<T>());
}

template<typename>
struct AlwaysFalse {
    static constexpr bool value = false;
};

/// What KVASIR_STATIC_ASSERT instantiates: a passing check is a class with one bool and nothing else to evaluate;
/// only Check<false, Gen> holds a static_assert, whose message Gen{}() is built then. Its value is true again, so the
/// caller's own assert does not fail a second time with a requirement line of template arguments.
template<bool Ok, typename Gen>
struct Check {
    static constexpr bool value = true;
};

template<typename Gen>
struct Check<false, Gen> {
    // a named constant: clang prints "due to requirement 'check'", not the generator's template arguments
    static constexpr bool check = AlwaysFalse<Gen>::value;
    static_assert(check,
                  Gen{}());
    static constexpr bool value = true;
};
}   // namespace Kvasir::Diagnostic

// `KVASIR_DETAIL_UNPAREN (X)` is `X`: how a parenthesised macro argument loses its parentheses
#define KVASIR_DETAIL_UNPAREN(...) __VA_ARGS__

// cond is spelled twice (the condition and lazy<>'s argument): pass a named constant. Gen in parentheses, it may hold
// commas: KVASIR_STATIC_ASSERT(ok, (RmwRefused<Address, ClearMask>), "the old text").
#if KVASIR_NAMED_DIAGNOSTICS
    #define KVASIR_STATIC_ASSERT(cond, Gen, fallback)                                             \
        static_assert(                                                                            \
          ::Kvasir::Diagnostic::Check<static_cast<bool>(cond), KVASIR_DETAIL_UNPAREN Gen>::value)
#else
    #define KVASIR_STATIC_ASSERT(cond, Gen, fallback) static_assert(cond, fallback)
#endif
