#pragma once
#include "Types.hpp"

#include <limits>

namespace Kvasir { namespace Register {
    constexpr unsigned maskFromRange(unsigned high,
                                     unsigned low) {
        return (0xFFFFFFFFUL >> (31U - (high - low))) << low;
    }

    template<typename... Is>
    constexpr unsigned maskFromRange(unsigned high,
                                     unsigned low,
                                     Is... args) {
        return maskFromRange(high, low) | maskFromRange(unsigned(args)...);
    }

    namespace Detail {
        using namespace MPL;

        constexpr unsigned maskStartsAt(unsigned mask) { return unsigned(std::countr_zero(mask)); }

        constexpr bool onlyOneBitSet(unsigned i) { return std::popcount(i) == 1; }

        constexpr unsigned orAllOf() { return 0; }

        constexpr unsigned orAllOf(unsigned l) { return l; }

        template<typename... Ts>
        constexpr unsigned orAllOf(unsigned l,
                                   unsigned r,
                                   Ts... args) {
            return l | r | orAllOf(args...);
        }

        template<typename T>
        struct ValueToUnsigned;

        template<typename T, T I>
        struct ValueToUnsigned<MPL::Value<T, I>> : MPL::Unsigned<unsigned(I)> {};

        template<typename T>
        struct GetFieldType;

        template<typename TAddress,
                 unsigned Mask,
                 typename TAccess,
                 typename TFieldType,
                 typename TAction>
        struct GetFieldType<Action<FieldLocation<TAddress, Mask, TAccess, TFieldType>, TAction>> {
            using type = TFieldType;
        };

        template<typename TAddress, unsigned Mask, typename TAccess, typename TFieldType>
        struct GetFieldType<FieldLocation<TAddress, Mask, TAccess, TFieldType>> {
            using type = TFieldType;
        };

        template<typename T>
        using GetFieldTypeT = typename GetFieldType<T>::type;

        template<typename T>
        struct IsWriteLiteral : std::false_type {};

        template<typename T>
        struct IsWriteRuntime : std::false_type {};

        template<typename T>
        struct IsFieldLocation : std::false_type {};

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct IsFieldLocation<FieldLocation<TAddress, Mask, Access, TFieldType>>
          : std::true_type {};

        template<typename T>
        struct IsWritable : std::false_type {};

        template<typename TAddress,
                 unsigned               Mask,
                 ReadActionType         RAction,
                 ModifiedWriteValueType WAction,
                 typename TFieldType>
        struct IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access<AccessType::readWrite, RAction, WAction>,
                                        TFieldType>> : std::true_type {};

        template<typename TAddress,
                 unsigned               Mask,
                 ReadActionType         RAction,
                 ModifiedWriteValueType WAction,
                 typename TFieldType>
        struct IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access<AccessType::writeOnly, RAction, WAction>,
                                        TFieldType>> : std::true_type {};

        template<typename T>
        struct IsSetToClear : std::false_type {};

        template<typename TAddress,
                 unsigned       Mask,
                 AccessType     AT,
                 ReadActionType RAction,
                 typename TFieldType>
        struct IsSetToClear<FieldLocation<TAddress,
                                          Mask,
                                          Access<AT, RAction, ModifiedWriteValueType::oneToClear>,
                                          TFieldType>> : std::true_type {};

        template<typename T, typename U>
        struct WriteLocationAndCompileTimeValueTypeAreSame : std::false_type {};

        template<typename AT, unsigned M, typename A, typename FT, FT V>
        struct WriteLocationAndCompileTimeValueTypeAreSame<FieldLocation<AT, M, A, FT>,
                                                           MPL::Value<FT, V>> : std::true_type {};

        // getters for specific parameters of an Action
        template<typename T>
        struct GetAddress;

        template<unsigned A, unsigned WIIZ, unsigned WIIO, typename TRegType, typename TMode>
        struct GetAddress<Address<A, WIIZ, WIIO, TRegType, TMode>> {
            static constexpr unsigned value                  = A;
            static constexpr unsigned writeIgnoredIfZeroMask = WIIZ;
            static constexpr unsigned writeIgnoredIfOneMask  = WIIO;
            static constexpr unsigned allBitsSetMask         = std::numeric_limits<TRegType>::max();

            static TRegType read() {
                TRegType volatile& reg = *reinterpret_cast<TRegType volatile*>(value);
                return reg;
            }

            static void write(TRegType i) {
                TRegType volatile& reg = *reinterpret_cast<TRegType volatile*>(value);
                reg                    = i;
            }

            using type = brigand::uint32_t<A>;
        };

        template<typename TAddress, unsigned Mask, typename TAccess, typename TFiledType>
        struct GetAddress<FieldLocation<TAddress, Mask, TAccess, TFiledType>>
          : GetAddress<TAddress> {};

        template<typename TFieldLocation, typename TAction>
        struct GetAddress<Action<TFieldLocation, TAction>> : GetAddress<TFieldLocation> {};

        template<typename T>
        struct GetFieldLocation;

        template<typename TLocation, typename TAction>
        struct GetFieldLocation<Action<TLocation, TAction>> {
            using type = TLocation;
        };

        // predicate returning result of left < right for RegisterOptions
        template<typename TLeft, typename TRight>
        struct RegisterActionLess;

        template<typename T1, typename U1, typename T2, typename U2>
        struct RegisterActionLess<Register::Action<T1, U1>, Register::Action<T2, U2>>
          : Bool<(GetAddress<T1>::value < GetAddress<T2>::value)> {};

        using RegisterActionLessP = Template<RegisterActionLess>;

        // predicate returns true if action is a read
        template<typename T>
        struct IsReadPred : std::false_type {};

        template<typename A>
        struct IsReadPred<Register::Action<A, ReadAction>> : std::true_type {};

        template<typename T>
        struct IsNotReadPred : std::integral_constant<bool, (!IsReadPred<T>::type::value)> {};

        // predicate returns true if action is a read
        template<typename T>
        struct IsRuntimeWritePred : std::false_type {};

        template<typename A>
        struct IsRuntimeWritePred<Register::Action<A, WriteAction>> : std::true_type {};

        template<typename T>
        struct IsNotRuntimeWritePred
          : std::integral_constant<bool, (!IsRuntimeWritePred<T>::type::value)> {};

        template<typename T>
        struct GetMask;

        // from FieldLocations
        template<typename Address, unsigned Mask, typename TAccess, typename ResultType>
        struct GetMask<FieldLocation<Address, Mask, TAccess, ResultType>>
          : Value<unsigned, Mask> {};

        // from Action
        template<typename TFieldLocation, typename TAction>
        struct GetMask<Action<TFieldLocation, TAction>> : GetMask<TFieldLocation> {};

    }   // namespace Detail
}}   // namespace Kvasir::Register
