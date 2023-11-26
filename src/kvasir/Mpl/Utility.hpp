#pragma once

#include "Types.hpp"
namespace Kvasir { namespace MPL {

    template<class T>
    void ignore(T const&) {}   // used to surpress compiler warning

    // type traits equivalents in case there is no standard library
    //#if (_MSC_VER == 1900)
    //		template<typename T>
    //		using VoidT = std::void_t<T>;
    //
    //		template<typename T>
    //		using AddRvalueReferenceT = std::add_rvalue_reference_t<T>;
    //
    //		template<typename T>
    //		AddRvalueReferenceT<T> Declval() ;
    //
    //#else
    // equivalent to std::void_t
    template<typename T>
    struct TypeSink {
        using type = void;
    };
    template<typename T>
    using VoidT = typename TypeSink<T>::type;

    template<class Ty>
    struct AddRvalueReference {   // add rvalue reference
        using type = Ty&&;
    };

    template<typename T>
    typename AddRvalueReference<T>::type Declval();
    //#endif
    // invert bool types
    template<typename T>
    struct Not {
        static_assert(AlwaysFalse<T>::value, "implausible type");
    };
    template<>
    struct Not<TrueType> : FalseType {};
    template<>
    struct Not<FalseType> : TrueType {};

    template<typename T>
    using NotT = typename Not<T>::type;

    // predecate returning result of left < right
    template<typename TLeft, typename TRight>
    struct Less {
        static_assert(AlwaysFalse<TLeft>::value, "implausible type");
    };
    template<typename T, T v1, T v2>
    struct Less<Value<T, v1>, Value<T, v2>> : Bool<(v1 < v2)> {};
    using LessP = Template<Less>;

    template<typename T>
    struct IsValue : FalseType {};
    template<typename T, T I>
    struct IsValue<MPL::Value<T, I>> : TrueType {};

    template<typename T>
    constexpr bool isValue(T) {
        return IsValue<T>::value;
    }

    // equivalent to std::is_same
    template<typename T, typename U>
    struct IsSame : FalseType {};

    template<typename T>
    struct IsSame<T, T> : TrueType {};

    template<typename T, typename U>
    using IsSameT = typename IsSame<T, U>::type;
    using IsSameP = Template<IsSame>;

    template<typename T>
    struct IsIntegral : FalseType {};
    template<>
    struct IsIntegral<long long> : TrueType {};
    template<>
    struct IsIntegral<unsigned long long> : TrueType {};
    template<>
    struct IsIntegral<long> : TrueType {};
    template<>
    struct IsIntegral<unsigned long> : TrueType {};
    template<>
    struct IsIntegral<int> : TrueType {};
    template<>
    struct IsIntegral<unsigned> : TrueType {};
    template<>
    struct IsIntegral<short> : TrueType {};
    template<>
    struct IsIntegral<unsigned short> : TrueType {};
    template<>
    struct IsIntegral<char> : TrueType {};
    template<>
    struct IsIntegral<signed char> : TrueType {};
    template<>
    struct IsIntegral<unsigned char> : TrueType {};
    template<>
    struct IsIntegral<bool> : TrueType {};

    template<typename T>
    using IsIntegralT = typename IsIntegral<T>::type;

    template<class T>
    struct RemoveConst {
        using type = T;
    };
    template<class T>
    struct RemoveConst<T const> {
        using type = T;
    };

    template<class T>
    struct RemoveVolatile {
        using type = T;
    };
    template<class T>
    struct RemoveVolatile<T volatile> {
        using type = T;
    };

    template<class T>
    struct RemoveCV {
        using type = typename RemoveVolatile<typename RemoveConst<T>::type>::type;
    };

    template<typename T>
    using RemoveCVT = typename RemoveCV<T>::type;

    // equivalent to std::enable_if
    template<bool, typename U = void>
    struct EnableIf {};
    template<typename U>
    struct EnableIf<true, U> {
        using type = U;
    };

    template<bool B, typename U = void>
    struct DisableIf : EnableIf<!B, U> {};

    template<bool B, typename U = void>
    using EnableIfT = typename EnableIf<B, U>::type;
    template<bool B, typename U = void>
    using DisableIfT = typename DisableIf<B, U>::type;

    // build a sequence of indices from 0 to N-1
    template<int N, typename... Is>
    struct BuildIndices : BuildIndices<N - 1, Int<N - 1>, Is...> {};

    template<typename... Is>
    struct BuildIndices<0, Is...> {
        using type = brigand::list<Is...>;
    };

    template<int N>
    using BuildIndicesT = typename BuildIndices<N>::type;

    template<typename TList>
    struct Size;
    template<typename... Ts>
    struct Size<brigand::list<Ts...>> : Int<int{sizeof...(Ts)}> {};
    template<typename TList>
    using SizeT = typename Size<TList>::type;

    template<bool B, typename T, typename U>
    struct Conditional {
        using type = U;
    };
    template<typename T, typename U>
    struct Conditional<true, T, U> {
        using type = T;
    };

    template<bool B, typename T, typename U>
    using ConditionalT = typename Conditional<B, T, U>::type;

    // helper recursively derives from a list of base classes
    template<typename TTemplateList, typename... Ts>
    struct DeriveFromTemplates {
        static_assert(
          AlwaysFalse<TTemplateList>::value,
          "implausible type, first parameter must be a MPL::List");
    };
    template<typename... Ts, typename... Us>
    struct DeriveFromTemplates<brigand::list<Ts...>, Us...> : ApplyTemplateT<Ts, Us...>... {};

}}   // namespace Kvasir::MPL
