#pragma once
#include "kvasir/Mpl/Algorithm.hpp"

#include <bit>
#include <cstddef>

namespace Kvasir { namespace Register {

    struct SequencePoint {
        using type = SequencePoint;
    };

    static constexpr SequencePoint sequencePoint{};

    // A register a partial write may read-modify-write.
    struct NormalMode {
        static constexpr unsigned mustSupplyMask          = 0;
        static constexpr bool     readHasSideEffect       = false;
        static constexpr unsigned writeOnlyNoIdentityMask = 0;
        static constexpr bool     writeOnlyRegister       = false;
        using Register = void;   // for compile errors only (Register/Diagnostic.hpp)
    };

    // A normal register that names itself for compile errors: svd_converter's NAMED_REGISTERS option
    // (off by default, it lengthens every Address type).
    template<typename TRegister>
    struct Named : NormalMode {
        using Register = TRegister;
    };

    // A register a read-modify-write would corrupt:
    // MustSupply - bits that read back something other than what must be written (a key: SCB
    // AIRCR.VECTKEY reads 0xFA05, needs 0x05FA), so every write names them; ReadSideEffect - a read
    // pops or clears something (I2C IC_DATA_CMD pops the RX FIFO), so only whole-register writes.
    // The generator emits it from the SVD: a field whose writeConstraint is a range of one value is
    // a key, a field with a readAction makes the register's read a side effect.
    // TRegister: the generated register struct itself (an incomplete type there, which a template argument may be),
    // read only when a compile error names the register and its fields.
    // WriteOnlyNoIdentity: write-only bits that a read cannot give back and that nobody classified (svd_converter
    // --write-only-mask): a partial write must name them, like a key.
    template<unsigned MustSupply,
             bool     ReadSideEffect,
             typename TRegister           = void,
             unsigned WriteOnlyNoIdentity = 0>
    struct RmwHazard {
        static constexpr unsigned mustSupplyMask          = MustSupply;
        static constexpr bool     readHasSideEffect       = ReadSideEffect;
        static constexpr unsigned writeOnlyNoIdentityMask = WriteOnlyNoIdentity;
        static constexpr bool     writeOnlyRegister       = false;
        using Register                                    = TRegister;
    };

    // A register with no readable field (svd_converter --write-only-registers=derived): a partial write never reads it
    // - there is nothing to keep - and writes the bits it does not name as 0, as a FULLREGISTER write of the same
    // value would.
    template<typename TRegister = void>
    struct WriteOnlyRegister : NormalMode {
        static constexpr bool writeOnlyRegister = true;
        using Register                          = TRegister;
    };

    template<unsigned A,
             unsigned WriteIgnoredIfZeroMask = 0,
             unsigned WriteIgnoredIfOneMask  = 0,
             typename TRegType               = unsigned,
             typename TMode                  = NormalMode>
    struct Address {
        using type    = Address<A, WriteIgnoredIfZeroMask, WriteIgnoredIfOneMask, TRegType, TMode>;
        using RegType = TRegType;
        using Mode    = TMode;
        static constexpr unsigned value = A;
        static_assert(A % sizeof(TRegType) == 0,
                      "register address is not aligned to the register's width");
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

    // A literal write that must not lose a concurrent update of another field of the register
    // (Register::atomic, AtomicFactories.hpp). How is the chip's business (ExecuteSeam: the RP
    // set/clear/xor aliases); the default masks interrupts around the read-modify-write.
    template<unsigned I>
    struct AtomicWriteLiteralAction {
        static constexpr unsigned value = I;
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
        static_assert(TMask != 0,
                      "a FieldLocation needs at least one bit");
        static_assert(static_cast<std::size_t>(std::bit_width(TMask))
                        <= sizeof(typename TAddress::RegType) * 8,
                      "the field's mask has bits beyond the register's width");
    };

    namespace Detail {
        using namespace MPL;

        constexpr unsigned positionOfFirstSetBit(unsigned in) {
            return unsigned(std::countr_zero(in));
        }

        // The two FieldLocation/Address sanity checks as predicates, for the tests.
        constexpr bool maskFitsRegister(unsigned    mask,
                                        std::size_t registerBytes) {
            return mask != 0 && static_cast<std::size_t>(std::bit_width(mask)) <= registerBytes * 8;
        }

        constexpr bool addressAligned(unsigned    address,
                                      std::size_t registerBytes) {
            return address % registerBytes == 0;
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
        auto operator[](T) const -> decltype(get<Detail::GetFieldLocationIndex<FieldTuple,
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
