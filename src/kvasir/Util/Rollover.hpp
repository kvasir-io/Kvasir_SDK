#pragma once

// Rollover<T>: a counter that wraps - a 32-bit timer word, a sequence number, an encoder count.
// Such a counter has no total order: a < b on the raw integers is right until the wrap and wrong
// after it. The questions it can answer are "how far apart" (a signed distance, operator-) and
// "is a behind b by less than half the range" (before/after). The relational operators are
// deleted so the wrong question does not compile.
//
// Standard headers only, so libraries that keep their core free of Kvasir can include it.
//
// "0 means unset" is not a position on a wrapping counter: a half-window comparison against it
// is wrong for half of all values. Use std::optional<Rollover<T>>.

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace Kvasir {

// Bits < the width of T is for counters narrower than their storage (SysTick's 24 bits in a
// uint32_t).
template<std::unsigned_integral T, unsigned Bits = sizeof(T) * 8>
struct Rollover {
    static_assert(Bits >= 2 && Bits <= sizeof(T) * 8,
                  "Rollover: 2 <= Bits <= width of T");

    using value_type = T;
    using Difference = std::make_signed_t<T>;

    static constexpr T mask = static_cast<T>(static_cast<T>(~T{0}) >> (sizeof(T) * 8 - Bits));

    constexpr Rollover() = default;

    constexpr explicit Rollover(T v) : v_{static_cast<T>(v & mask)} {}

    [[nodiscard]] constexpr T value() const { return v_; }

    // Signed distance from rhs to lhs, in [-2^(Bits-1), 2^(Bits-1)). Meaningful only while the two
    // are less than half the range apart; nothing can check that.
    [[nodiscard]] friend constexpr Difference operator-(Rollover lhs,
                                                        Rollover rhs) {
        constexpr unsigned shift = sizeof(T) * 8 - Bits;
        T const            d     = static_cast<T>(static_cast<T>(lhs.v_ - rhs.v_) & mask);
        // sign-extend from Bits: the counter's top bit into T's, then back down arithmetically
        return static_cast<Difference>(static_cast<Difference>(static_cast<T>(d << shift))
                                       >> shift);
    }

    template<std::integral D>
    [[nodiscard]] friend constexpr Rollover operator+(Rollover lhs,
                                                      D        rhs) {
        return Rollover{static_cast<T>(lhs.v_ + static_cast<T>(rhs))};
    }

    template<std::integral D>
    [[nodiscard]] friend constexpr Rollover operator-(Rollover lhs,
                                                      D        rhs) {
        return Rollover{static_cast<T>(lhs.v_ - static_cast<T>(rhs))};
    }

    constexpr Rollover& operator++() {
        v_ = static_cast<T>(static_cast<T>(v_ + 1U) & mask);
        return *this;
    }

    constexpr Rollover operator++(int) {
        auto const old = *this;
        ++*this;
        return old;
    }

    template<std::integral D>
    constexpr Rollover& operator+=(D d) {
        return *this = *this + d;
    }

    template<std::integral D>
    constexpr Rollover& operator-=(D d) {
        return *this = *this - d;
    }

    [[nodiscard]] friend constexpr bool operator==(Rollover,
                                                   Rollover) = default;

    friend bool operator<(Rollover,
                          Rollover)
      = delete(
        "a wrapping counter has no total order: use before()/after(), which compare within "
        "half the range");
    friend bool operator>(Rollover,
                          Rollover)
      = delete("a wrapping counter has no total order: use before()/after()");
    friend bool operator<=(Rollover,
                           Rollover)
      = delete("a wrapping counter has no total order: use !after()");
    friend bool operator>=(Rollover,
                           Rollover)
      = delete("a wrapping counter has no total order: use !before()");

private:
    T v_{};
};

template<std::unsigned_integral T>
Rollover(T) -> Rollover<T>;

// a is behind b: b is reached from a by walking forward less than half the range. Two values
// exactly half the range apart are each before the other (as TCP's seq_lt); a caller that can get
// that far apart has the wrong counter width.
template<typename T,
         unsigned B>
[[nodiscard]] constexpr bool before(Rollover<T,
                                             B> a,
                                    Rollover<T,
                                             B> b) {
    return (a - b) < 0;
}

template<typename T,
         unsigned B>
[[nodiscard]] constexpr bool after(Rollover<T,
                                            B> a,
                                   Rollover<T,
                                            B> b) {
    return (a - b) > 0;
}

// x lies on the way from first to last, walking forward (both ends included). A membership test
// over any span up to the full range, no half-window limit.
template<typename T,
         unsigned B>
[[nodiscard]] constexpr bool inWindow(Rollover<T,
                                               B> x,
                                      Rollover<T,
                                               B> first,
                                      Rollover<T,
                                               B> last) {
    using R = Rollover<T, B>;
    return static_cast<T>(static_cast<T>(x.value() - first.value()) & R::mask)
        <= static_cast<T>(static_cast<T>(last.value() - first.value()) & R::mask);
}

}   // namespace Kvasir
