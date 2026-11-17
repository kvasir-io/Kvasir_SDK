#pragma once
// Kvasir::PaddingFree<T>: every byte of a T belongs to a value - no padding between or behind its members.
//
// What a CRC, a hash or a byte compare over an object's bytes needs: padding bits have unspecified values
// ([basic.types.general]), a member-wise copy need not carry them, and a record whose CRC was taken over them can
// fail it after the copy. Unlike std::has_unique_object_representations it takes floating-point members (a float has
// no padding, but two representations of one value).
//
//     struct Calibration { std::uint16_t offset; std::uint16_t flags; float gain; };
//     static_assert(Kvasir::PaddingFree<Calibration>);
//
// A struct that fails names its padding: a `std::uint8_t reserved[3]{}` where the compiler put it.
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

namespace Kvasir {
/// For a class the check cannot look into (constructors, private members, a base with members): its author's word
/// that it has no padding. Specialise it next to the class.
template<typename T>
inline constexpr bool declaredPaddingFree = false;

namespace Detail {
    template<typename T>
    struct PaddingFreeOf;

    // binary32, binary64 and binary128 fill their bytes; the x87 extended format leaves six unused
    template<typename T>
    inline constexpr bool floatFillsItsBytes
      = std::numeric_limits<T>::digits == 24 || std::numeric_limits<T>::digits == 53
     || std::numeric_limits<T>::digits == 113;

#if __cpp_structured_bindings >= 202411L
    // The members of an aggregate, by structured binding: their sizes add up to the aggregate's and each is without
    // padding itself. Never called: the answer is the return type. Needs structured-binding packs (P1061): arm GCC 15
    // has none (__cpp_structured_bindings 202403L) and does not even parse the body, so there an aggregate is
    // padding-free only by has_unique_object_representations or its author's word -- one with a float member is not
    // (never wrong towards "padding-free"; the compilers with packs walk it).
    template<typename T>
    auto aggregateFilledByMembers(T const& value) {
        auto const& [... member] = value;
        return std::bool_constant<((sizeof(member) + ... + std::size_t{0}) == sizeof(T))
                                  && (PaddingFreeOf<std::remove_cvref_t<decltype(member)>>::value
                                      && ...)>{};
    }
#endif

    template<typename T>
    struct PaddingFreeOf : std::false_type {};

    template<typename T>
        requires std::has_unique_object_representations_v<T> || declaredPaddingFree<T>
    struct PaddingFreeOf<T> : std::true_type {};

    template<typename T>
        requires std::is_floating_point_v<T>
    struct PaddingFreeOf<T> : std::bool_constant<floatFillsItsBytes<T>> {};

    template<typename T, std::size_t N>
        requires(!std::has_unique_object_representations_v<T[N]>)
    struct PaddingFreeOf<T[N]> : PaddingFreeOf<std::remove_cv_t<T>> {};

#if __cpp_structured_bindings >= 202411L
    template<typename T>
        requires(std::is_aggregate_v<T> && std::is_class_v<T>
                 && !std::has_unique_object_representations_v<T> && !declaredPaddingFree<T>)
    struct PaddingFreeOf<T> : decltype(aggregateFilledByMembers(std::declval<T const&>())){};
#endif
}   // namespace Detail

template<typename T>
concept PaddingFree = Detail::PaddingFreeOf<std::remove_cv_t<T>>::value;

namespace Detail { namespace PaddingFreeChecks {
    struct Words {
        unsigned a;
        unsigned b;
    };

    struct WithFloat {
        unsigned a;
        float    b;
    };

    struct Padded {
        unsigned char a;
        unsigned      b;
    };

    static_assert(PaddingFree<Words> && !PaddingFree<Padded>,
                  "words fill their bytes, a padded struct does not");
#if __cpp_structured_bindings >= 202411L
    static_assert(PaddingFree<WithFloat>,
                  "the member walk takes a float member");
#else
    static_assert(!PaddingFree<WithFloat>,
                  "without structured-binding packs a float member is not walked: no");
#endif
}}   // namespace Detail::PaddingFreeChecks
}   // namespace Kvasir
