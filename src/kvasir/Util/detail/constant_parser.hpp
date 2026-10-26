#pragma once

#include <cstddef>

namespace Kvasir { namespace literals { namespace detail {
    // Not constexpr: reaching one of these while a literal is parsed is the compile error, and the
    // function's name is the message (the Prescaler idiom, Util/Prescaler.hpp; `throw` is refused under
    // -fno-exceptions by clang and gcc alike).
    inline int literalCharacterIsNotADigit(char c) { return c; }

    inline int literalDigitIsInvalidForItsBase(char c) { return c; }

    inline int integerLiteralOverflowsItsParseType() { return 0; }

    consteval int digitValue(char c) {
        if(c >= '0' && c <= '9') { return c - '0'; }
        if(c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if(c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return literalCharacterIsNotADigit(c);
    }

    // T is long long or unsigned long long (literals.hpp); the suffix's own range is checked after this,
    // by validate_value. A lone 0 is decimal; 0x/0X hex, 0b/0B binary, any other leading 0 octal;
    // digit separators are skipped wherever they stand.
    template<typename T,
             char... Cs>
    consteval T parse() {
        constexpr char        s[]{Cs...};
        constexpr std::size_t n    = sizeof...(Cs);
        std::size_t           i    = 0;
        int                   base = 10;
        if(n > 1 && s[0] == '0') {
            if(s[1] == 'x' || s[1] == 'X') {
                base = 16;
                i    = 2;
            } else if(s[1] == 'b' || s[1] == 'B') {
                base = 2;
                i    = 2;
            } else {
                base = 8;
                i    = 1;
            }
        }
        T v{};
        for(; i < n; ++i) {
            if(s[i] == '\'') { continue; }
            int const d = digitValue(s[i]);
            if(d >= base) { literalDigitIsInvalidForItsBase(s[i]); }
            if(__builtin_mul_overflow(v, static_cast<T>(base), &v)
               || __builtin_add_overflow(v, static_cast<T>(d), &v))
            {
                integerLiteralOverflowsItsParseType();
            }
        }
        return v;
    }
}}}   // namespace Kvasir::literals::detail
