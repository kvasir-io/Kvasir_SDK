#pragma once
#include "kvasir/Mpl/Algorithm.hpp"

#include <bit>

namespace Kvasir { namespace Register {

    struct SequencePoint {
        using type = SequencePoint;
    };

    static constexpr SequencePoint sequencePoint{};

    template<int I>
    struct IsolatedByte {
        static constexpr int value = I;
        using type                 = IsolatedByte<I>;
    };

    namespace Isolated {
        static constexpr IsolatedByte<0> byte0{};
        static constexpr IsolatedByte<1> byte1{};
        static constexpr IsolatedByte<2> byte2{};
        static constexpr IsolatedByte<3> byte3{};
    }   // namespace Isolated

    struct PushableMode {};

    struct NormalMode {};

    struct SpecialReadMode {};

    template<unsigned A,
             unsigned WriteIgnoredIfZeroMask = 0,
             unsigned WriteIgnoredIfOneMask  = 0,
             typename TRegType               = unsigned,
             typename TMode                  = NormalMode>
    struct Address {
        using type    = Address<A, WriteIgnoredIfZeroMask, WriteIgnoredIfOneMask, TRegType, TMode>;
        using RegType = TRegType;
        static constexpr unsigned value = A;
    };

    // write a compile time known value
    template<unsigned I>
    struct WriteLiteralAction {
        static constexpr unsigned value = I;
    };

    // write a compile time and runtime time known value
    template<unsigned I>
    struct WriteRuntimeAndLiteralAction {
        static constexpr unsigned value = I;
        unsigned                  value_;
    };

    // write a run time known value
    struct WriteAction {
        unsigned value_;
    };

    // read
    struct ReadAction {};

    // xor a compile time known mask
    template<unsigned I>
    struct XorLiteralAction {
        static constexpr unsigned value = I;
    };

    // xor a run time known value
    struct XorAction {
        unsigned value_;
    };

    template<typename TLocation, typename TAction>
    struct Action : TAction {
        static constexpr bool isAction = true;

        template<typename... Ts>
        constexpr Action(
          Ts... args)   //NOLINT(hicpp-explicit-constructor, hicpp-explicit-conversions)
          : TAction{args...} {}

        using type = Action<TLocation, TAction>;

        using location = TLocation;
    };

    enum class ModifiedWriteValueType {
        normal,
        oneToClear,
        oneToSet,
        oneToToggle,
        zeroToClear,
        zeroToSet,
        zeroToToggle,
        clear,
        set,
        modify
    };

    enum class ReadActionType { normal, clear, set, modify, modifyExternal };

    enum class AccessType { readOnly, writeOnly, readWrite, writeOnce, readWriteOnce };

    template<AccessType,
             ReadActionType         = ReadActionType::normal,
             ModifiedWriteValueType = ModifiedWriteValueType::normal>
    struct Access {};

    using ReadWriteAccess = Access<AccessType::readWrite>;
    using ReadOnlyAccess  = Access<AccessType::readOnly>;
    using WriteOnlyAccess = Access<AccessType::writeOnly>;
    using ROneToClearAccess
      = Access<AccessType::readWrite, ReadActionType::normal, ModifiedWriteValueType::oneToClear>;

    template<typename TAddress,
             unsigned TMask,
             typename TAccess    = ReadWriteAccess,
             typename TFieldType = unsigned>
    struct FieldLocation {
        using type                 = FieldLocation<TAddress, TMask, TAccess, TFieldType>;
        using DataType             = TFieldType;
        using Access               = TAccess;
        static constexpr auto Mask = TMask;
    };

    template<typename T, typename U>
    struct FieldLocationPair {
        using type = FieldLocationPair<T, U>;
    };

    namespace Detail {
        using namespace MPL;

        constexpr unsigned positionOfFirstSetBit(unsigned in) {
            return unsigned(std::countr_zero(in));
        }
    }   // namespace Detail

    template<typename TFieldLocation, typename TFieldLocation::DataType Value>
    struct FieldValue {
        using type = FieldValue<TFieldLocation, Value>;

        operator typename TFieldLocation::
          DataType()   //NOLINT(hicpp-explicit-constructor, hicpp-explicit-conversions)
          const {
            return Value;
        }
    };
    template<typename TAddresses, typename TFieldLocation>
    struct FieldTuple;   // see below for implementation in specialization

    namespace Detail {
        template<typename Object, typename TFieldLocation>
        struct GetFieldLocationIndex;

        template<typename TA, typename TLocations, typename TFieldLocation>
        struct GetFieldLocationIndex<FieldTuple<TA, TLocations>, TFieldLocation>
          : MPL::Find<TLocations, TFieldLocation> {};

        template<typename Object, typename TFieldLocation>
        using GetFieldLocationIndexT = typename GetFieldLocationIndex<Object, TFieldLocation>::type;
    }   // namespace Detail

    template<uint32_t... Is,
             typename... TAs,
             unsigned... Masks,
             typename... TAccesss,
             typename... TRs>
    struct FieldTuple<brigand::list<brigand::uint32_t<Is>...>,
                      brigand::list<FieldLocation<TAs, Masks, TAccesss, TRs>...>> {
        std::array<unsigned, sizeof...(Is)> value_;

        template<std::size_t Index>
        brigand::at_c<brigand::list<TRs...>,
                      Index>
        get() const {
            using namespace MPL;
            using Address = brigand::uint32_t<brigand::at_c<brigand::list<TAs...>, Index>::value>;
            static constexpr unsigned index
              = sizeof...(Is)
              - brigand::size<brigand::find<brigand::list<brigand::uint32_t<Is>...>,
                                            std::is_same<Address, brigand::_1>>>::value;
            using ResultType = brigand::at_c<brigand::list<TRs...>, Index>;
            static constexpr unsigned mask
              = brigand::at_c<brigand::list<brigand::uint32_t<Masks>...>, Index>::value;
            static constexpr unsigned shift = Detail::positionOfFirstSetBit(mask);
            unsigned                  r     = (value_[index] & mask) >> shift;
            return ResultType(r);
        }

        template<typename T>
        auto operator[](T) -> decltype(get<Detail::GetFieldLocationIndex<FieldTuple,
                                                                         T>::value>()) {
            return get<Detail::GetFieldLocationIndex<FieldTuple, T>::value>();
        }

        template<typename... T>
        static constexpr unsigned getFirst(unsigned i,
                                           T...) {
            return i;
        }

        struct DoNotUse {
            template<typename T>
            explicit DoNotUse(T) {}
        };

        // implicitly convertible to the field type only if there is just one field
        using ConvertableTo = typename std::conditional<(sizeof...(TRs) == 1),
                                                        brigand::at_c<brigand::list<TRs...>, 0>,
                                                        DoNotUse>::type;

        operator ConvertableTo() {   //NOLINT(hicpp-explicit-conversions)
            static constexpr unsigned mask  = getFirst(Masks...);
            static constexpr unsigned shift = Detail::positionOfFirstSetBit(mask);
            return ConvertableTo((value_[0] & mask) >> shift);
        }
    };

    template<>
    struct FieldTuple<brigand::list<>, brigand::list<>> {};

    template<std::size_t I,
             typename TFieldTuple>
    auto get(TFieldTuple o) -> decltype(o.template get<I>()) {
        return o.template get<I>();
    }

    template<typename T,
             typename TFieldTuple>
    auto get(T,
             TFieldTuple o) -> decltype(o.template get<Detail::GetFieldLocationIndex<TFieldTuple,
                                                                                     T>::value>()) {
        return o.template get<Detail::GetFieldLocationIndex<TFieldTuple, T>::value>();
    }

    template<typename TFieldTuple,
             typename TLocation,
             typename TLocation::DataType Value>
    bool operator==(TFieldTuple const& f,
                    FieldValue<TLocation,
                               Value> const) {
        return get(TLocation{}, f) == Value;
    }
}}   // namespace Kvasir::Register
