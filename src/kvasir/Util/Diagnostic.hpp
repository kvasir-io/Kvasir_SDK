#pragma once
// Compile errors that name what they are about: the register and field, the peripheral, the resource.
//
//     static constexpr bool fits = literalFits(Mask, Value);
//     KVASIR_STATIC_ASSERT(fits, (Register::Diagnostic::LiteralTooWide<RegisterTraits<Address>, Mask, Value>));
//
// A message is a C++26 user-generated static_assert message (P2741: an object with constant size()
// and data()) built by a generator - a class template whose consteval operator() returns a Text.
//
// THE RULE: build a message only when the assert fails. clang evaluates a user-generated message even
// for a PASSING assert (gcc does not), at roughly 30x the compile cost of a string literal.
// KVASIR_STATIC_ASSERT therefore asserts Check<cond, Gen>::value: a passing check is a class with one
// bool, only Check<false, Gen> holds the static_assert whose message is Gen{}().
#include "kvasir/Mpl/Types.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Kvasir::Prescaler {
namespace detail {
    // not constexpr: reaching it in a constant expression is the compile error
    inline std::uint64_t prescalerArithmeticOverflowsSixtyFourBits(std::uint64_t a,
                                                                   std::uint64_t b) {
        return a * b;
    }
}   // namespace detail

/// a * b; a product that would overflow 64 bits is a compile error in a constant expression, never a wrap. Defined
/// here, not in Prescaler.hpp (which includes this header), because Text::fixed1 needs it too.
constexpr std::uint64_t mulChecked(std::uint64_t a,
                                   std::uint64_t b) {
    std::uint64_t r{};
    if(__builtin_mul_overflow(a, b, &r)) {
        return detail::prescalerArithmeticOverflowsSixtyFourBits(a, b);
    }
    return r;
}
}   // namespace Kvasir::Prescaler

namespace Kvasir::Diagnostic {

/// A string literal as a template argument: `Trace::Ring<"usb", ...>`, `assertInTolerance<..., "UART0">()`.
template<std::size_t N>
struct FixedString {
    std::array<char, N> text{};

    static constexpr std::size_t length = N - 1;

    consteval FixedString(char const (&s)[N]) {
        for(std::size_t i = 0; i != N; ++i) { text[i] = s[i]; }
    }

    [[nodiscard]] constexpr std::string_view view() const { return {text.data(), length}; }
};

// a string literal as a template argument deduces N (and keeps clang's -Wctad-maybe-unsupported quiet, which a
// firmware build treats as an error)
template<std::size_t N>
FixedString(char const (&)[N]) -> FixedString<N>;

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

    /// plus: a "+" before a value >= 0 ("+17253", "+0"), as a ppm error is written
    constexpr Text& sdec(std::int64_t v,
                         bool         plus = false) {
        if(plus && v >= 0) { *this << "+"; }
        std::array<char, 24> b{};
        auto const           r = std::to_chars(b.data(), b.data() + b.size(), v);
        return *this << std::string_view{b.data(), static_cast<std::size_t>(r.ptr - b.data())};
    }

    /// num / den with one decimal, rounded to nearest: "117187.5"
    constexpr Text& fixed1(std::uint64_t num,
                           std::uint64_t den) {
        auto const tenths = (Prescaler::mulChecked(num, 10) + den / 2) / den;
        dec(tenths / 10) << ".";
        return dec(tenths % 10);
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
        unsigned low = 0;
        while(((mask >> low) & 1U) == 0) { ++low; }
        unsigned high = low;
        while(high < 31 && ((mask >> (high + 1)) & 1U) != 0) { ++high; }
        std::uint32_t const run
          = high == 31 ? ~0U << low : ((1U << (high + 1)) - 1U) & (~0U << low);
        if(run != mask) { return hex(mask); }
        dec(high);
        if(high != low) {
            *this << ":";
            dec(low);
        }
        return *this;
    }
};

/// A generator's text as an object with size() / data() / view(): golden tests, and a message type by name.
template<typename Gen>
struct Message {
    static constexpr Text storage = Gen{}();

    [[nodiscard]] constexpr std::size_t size() const { return storage.size(); }

    [[nodiscard]] constexpr char const* data() const { return storage.data(); }

    [[nodiscard]] constexpr std::string_view view() const { return storage.view(); }
};

namespace Detail {
    // the text after `key` in a __PRETTY_FUNCTION__: clang "... [T = X]", gcc "... [with T = X; U = ...]" (gcc's
    // "; " is the separator before the next binding; a "]" inside X, as in "int [4]", is not the end)
    consteval std::string_view prettyArgument(std::string_view f,
                                              std::string_view key) {
        auto const b = f.find(key);
        if(b == std::string_view::npos) { return f; }
        auto const from = b + key.size();
#ifdef __clang__
        auto const e = f.rfind(']');
#else
        auto e = f.find("; ", from);
        if(e == std::string_view::npos) { e = f.rfind(']'); }
#endif
        return f.substr(from, e - from);
    }
}   // namespace Detail

/// "Kvasir::I2C::Master<HW::I2c0Config, 3>": T's name as the compiler spells it, from __PRETTY_FUNCTION__.
/// Used at run time, the whole __PRETTY_FUNCTION__ string goes into the image (the view points into it).
template<typename T>
consteval std::string_view typeName() {
    return Detail::prettyArgument(__PRETTY_FUNCTION__, "T = ");
}

/// "Ns::Class<Args>::fn": a function's (or any value's) name as the compiler spells it, without clang's "&".
template<auto V>
consteval std::string_view valueName() {
    auto n = Detail::prettyArgument(__PRETTY_FUNCTION__, "V = ");
    if(!n.empty() && n.front() == '&') { n.remove_prefix(1); }
    return n;
}

/// "Ns::Class<A<B>>::fn" -> "Class::fn": template argument lists (nested) dropped, the last `parts` names kept.
/// Writes into out (room for name.size() chars), returns the length.
constexpr std::size_t lastNames(std::string_view name,
                                char*            out,
                                std::size_t      parts = 2) {
    std::size_t n     = 0;
    int         depth = 0;
    for(char const c : name) {
        if(c == '<') {
            ++depth;
        } else if(c == '>') {
            --depth;
        } else if(depth == 0) {
            out[n++] = c;
        }
    }
    std::string_view const flat{out, n};
    std::size_t            pos = n;
    std::size_t            cut = 0;
    for(std::size_t i = 0; i != parts; ++i) {
        if(pos == 0) { return n; }
        auto const p = flat.rfind("::", pos - 1);
        if(p == std::string_view::npos) { return n; }
        cut = p + 2;
        pos = p;
    }
    // shift the kept names to the front, zero the rest (a copied buffer then holds nothing but the name)
    for(std::size_t i = 0; i != n; ++i) { out[i] = i + cut < n ? out[i + cut] : '\0'; }
    return n - cut;
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
    static constexpr bool check = MPL::AlwaysFalse<Gen>::value;
    static_assert(check,
                  Gen{}());
    static constexpr bool value = true;
};
}   // namespace Kvasir::Diagnostic

// `KVASIR_DETAIL_UNPAREN (X)` is `X`: how a parenthesised macro argument loses its parentheses
#define KVASIR_DETAIL_UNPAREN(...) __VA_ARGS__

// cond is spelled twice (the condition and Check<>'s argument): pass a named constant. Gen in parentheses, it may hold
// commas: KVASIR_STATIC_ASSERT(ok, (RmwRefused<Address, ClearMask>)).
#define KVASIR_STATIC_ASSERT(cond, Gen)                                                       \
    static_assert(                                                                            \
      ::Kvasir::Diagnostic::Check<static_cast<bool>(cond), KVASIR_DETAIL_UNPAREN Gen>::value)
