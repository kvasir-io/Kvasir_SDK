#pragma once

#include <utility>

namespace Kvasir {

namespace Detail {
    template<std::size_t Size, typename = void>
    struct GetBitFieldValueType {
        using type = std::uint32_t;
    };

    template<std::size_t Size>
    struct GetBitFieldValueType<Size, std::enable_if_t<(Size <= 7)>> {
        using type = std::uint8_t;
    };

    template<std::size_t Size>
    struct GetBitFieldValueType<Size, std::enable_if_t<(Size > 7 && Size <= 15)>> {
        using type = std::uint16_t;
    };

    template<std::size_t Size>
    using GetIndexTypeT = typename GetBitFieldValueType<Size, void>::type;

}   // namespace Detail

template<std::size_t High, std::size_t Low>
using BitFieldValueType = Detail::GetIndexTypeT<High - Low>;

template<std::size_t                  High,
         std::size_t                  Low,
         BitFieldValueType<High, Low> Min = std::numeric_limits<BitFieldValueType<High, Low>>::min()
                                          & Kvasir::Register::maskFromRange(High - Low, 0),
         BitFieldValueType<High, Low> Max = std::numeric_limits<BitFieldValueType<High, Low>>::max()
                                          & Kvasir::Register::maskFromRange(High - Low, 0),
         typename assignType = BitFieldValueType<High, Low>>
struct BitField {
    static_assert(32 >= High - Low);
    static_assert(High >= Low);
    static_assert(Max >= Min);
    static_assert(Kvasir::Register::maskFromRange(High - Low,
                                                  0)
                  >= Max);

    using value_Type = BitFieldValueType<High, Low>;

    constexpr BitField(
      assignType v_)   //NOLINT(hicpp-explicit-constructor, hicpp-explicit-conversions)
      : v{sanitize(static_cast<value_Type>(v_))} {}

    constexpr
    operator assignType() const {   //NOLINT(hicpp-explicit-constructor, hicpp-explicit-conversions)
        return static_cast<assignType>(v);
    }

    value_Type asValue() const { return v; }

    value_Type asAssign() const { return static_cast<assignType>(v); }

    template<std::size_t N>
    constexpr explicit BitField(std::array<std::byte,
                                           N> const& a)
      : v{sanitize(extract(a))} {
        static_assert(N * 8 > High);
    }

    template<std::size_t N>
    static constexpr bool isValid(std::array<std::byte,
                                             N> const& a) {
        static_assert(N * 8 > High);
        auto av        = extract(a);
        auto sanitized = sanitize(av);
        return av == sanitized;
    }

    template<std::size_t N>
    constexpr void combineInto(std::array<std::byte,
                                          N>& a) const {
        static_assert(N * 8 > High);

        unrollCombine(a);
        /*for(std::size_t n = Start; n < End + 1; ++n) {
            a[n] &= ~std::byte(Masks[n - Start]);
            value_Type lshift = n == Start ? FrontMaskOffset : 0;
            value_Type rshift = n == End ? End == Start ? 0 : (End - Start) * 8 - (FrontMaskOffset + BackMaskOffset) :
        0; a[n] |= std::byte(((v << lshift) >> rshift) & Masks[n - Start]);
        }*/
    }

    constexpr bool operator==(BitField const& rhs) const { return v == rhs.v; }

private:
    constexpr void doTheCombine(value_Type lshift,
                                value_Type rshift,
                                value_Type Mask,
                                std::byte* vv) const {
        *vv &= ~std::byte(Mask);
        *vv |= std::byte(((unsigned(v) << unsigned(lshift)) >> unsigned(rshift)) & unsigned(Mask));
    }

    template<std::size_t N,
             std::size_t... Is>
    constexpr void unrollCombineImpl(std::array<std::byte,
                                                N>& a,
                                     std::index_sequence<Is...>) const {
        constexpr auto EndShift = []() {
            if constexpr(End == Start) {
                return 0;
            } else {
                return (End - Start) * 8 - (FrontMaskOffset + BackMaskOffset);
            }
        }();

        ((doTheCombine(Is == 0 ? FrontMaskOffset : 0,
                       Start + Is == End ? EndShift : 0,
                       Masks[Is],
                       std::addressof(a[Start + Is]))),
         ...);
    }

    template<std::size_t N>
    constexpr void unrollCombine(std::array<std::byte,
                                            N>& a) const {
        return unrollCombineImpl(a, std::make_index_sequence<(End - Start) + 1>());
    }

    constexpr static value_Type mask(std::size_t n) {
        if(n * 8 > (High / 8) * 8) {
            return 0;
        }

        if constexpr(Low >= 8) {
            if((Low / 8) * 8 > n * 8) {
                return 0;
            }
        }

        std::size_t high = High;
        std::size_t low  = Low;

        if(n * 8 > High) {
            high = 7;
        } else {
            high -= n * 8;
        }

        if(n * 8 > Low) {
            low = 0;
        } else {
            low -= n * 8;
        }

        return Kvasir::Register::maskFromRange(high, low) & 0xFFU;
    }

    static constexpr auto Start = Low / 8;
    static constexpr auto End   = High / 8;

    template<std::size_t... Is>
    static constexpr std::array<value_Type,
                                (End - Start) + 1>
    generateMaskImpl(std::index_sequence<Is...>) {
        return {mask(Is + Start)...};
    }

    static constexpr auto generateMask() {
        return generateMaskImpl(std::make_index_sequence<(End - Start) + 1>());
    }

    constexpr static value_Type doTheExtract(value_Type lshift,
                                             value_Type rshift,
                                             value_Type Mask,
                                             std::byte  vv) {
        return value_Type(((std::to_integer<unsigned>(vv) & unsigned(Mask)) >> unsigned(lshift))
                          << unsigned(rshift));
    }

    template<std::size_t N,
             std::size_t... Is>
    constexpr static value_Type unrollExtractImpl(std::array<std::byte,
                                                             N> const& a,
                                                  std::index_sequence<Is...>) {
        auto calcRshift = [](auto n) -> value_Type {
            if(n == 0) {
                return 0;
            }
            return value_Type((n * 8) - FrontMaskOffset);
        };
        return (
          (doTheExtract(Is == 0 ? FrontMaskOffset : 0, calcRshift(Is), Masks[Is], a[Start + Is]))
          | ...);
    }

    template<std::size_t N>
    constexpr static value_Type unrollExtract(std::array<std::byte,
                                                         N> const& a) {
        return unrollExtractImpl(a, std::make_index_sequence<(End - Start) + 1>());
    }

    template<std::size_t N>
    constexpr static value_Type extract(std::array<std::byte,
                                                   N> const& a) {
        static_assert(N * 8 > High);
        return unrollExtract(a);
        /*
        value_Type ret = 0;
        for(std::size_t n = Start; n < End + 1; ++n) {
            value_Type lshift = n == Start ? FrontMaskOffset : 0;
            value_Type rshift = n == Start ? 0 : ((n - Start) * 8) - FrontMaskOffset;
            ret |= ((std::to_integer<value_Type>(a[n]) & Masks[n - Start]) >> lshift) << rshift;
        }

        return ret;*/
    }

    constexpr static value_Type sanitize(value_Type v_) {
        return std::clamp<value_Type>(v_, Min, Max);
    }

    value_Type v;

    static constexpr auto Masks           = generateMask();
    static constexpr auto FrontMaskOffset = Kvasir::Register::Detail::maskStartsAt(Masks.front());
    static constexpr auto BackMaskOffset  = Kvasir::Register::Detail::maskStartsAt(Masks.back());
};

}   // namespace Kvasir
