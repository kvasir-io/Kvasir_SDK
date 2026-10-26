#pragma once
#include "Mmio.hpp"
#include "Types.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace Kvasir { namespace Register {
    constexpr unsigned maskFromRange(unsigned high,
                                     unsigned low) {
        return (0xFFFFFFFFU >> (31U - (high - low))) << low;
    }

    template<typename... Is>
    constexpr unsigned maskFromRange(unsigned high,
                                     unsigned low,
                                     Is... args) {
        return maskFromRange(high, low) | maskFromRange(unsigned(args)...);
    }

    namespace Detail {
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
        struct IsFieldLocation : std::false_type {};

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct IsFieldLocation<FieldLocation<TAddress, Mask, Access, TFieldType>>
          : std::true_type {};

        template<typename T>
        struct AccessTypeOf;

        template<AccessType AT, ReadActionType RAT, ModifiedWriteValueType MWV>
        struct AccessTypeOf<Access<AT, RAT, MWV>> : std::integral_constant<AccessType, AT> {};

        constexpr bool accessWritable(AccessType at) { return at != AccessType::readOnly; }

        constexpr bool accessReadable(AccessType at) {
            return at != AccessType::writeOnly && at != AccessType::writeOnce;
        }

        // Every write factory (literal and runtime) asserts this; a write to a read-only field
        // is ignored by the hardware and would otherwise compile.
        template<typename T>
        struct IsWritable : std::false_type {};

        template<typename TAddress, unsigned Mask, typename TAccess, typename TFieldType>
        struct IsWritable<FieldLocation<TAddress, Mask, TAccess, TFieldType>>
          : std::bool_constant<accessWritable(AccessTypeOf<TAccess>::value)> {};

        // read() asserts this; a write-only field reads back as junk.
        template<typename T>
        struct IsReadable : std::false_type {};

        template<typename TAddress, unsigned Mask, typename TAccess, typename TFieldType>
        struct IsReadable<FieldLocation<TAddress, Mask, TAccess, TFieldType>>
          : std::bool_constant<accessReadable(AccessTypeOf<TAccess>::value)> {};

        // A literal fits its field when it has no bits above the field's width: the shift
        // into place would otherwise push them out the top, past the post-shift mask check.
        constexpr bool literalFits(unsigned mask,
                                   unsigned value) {
            return value <= (mask >> maskStartsAt(mask));
        }

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
                                                           std::integral_constant<FT, V>>
          : std::true_type {};
    }   // namespace Detail

    // A register's facts, from its Address, a FieldLocation of it or an Action on it: address, RegType, the
    // write-ignored-if-zero/-one masks, allBitsSetMask, mustSupplyMask, readHasSideEffect, writeOnlyRegister,
    // writeBackActsMask (from Mode::semantics), Mode; read()/write() of the whole word (always inlined, through Mmio).
    // A register picked at run time is read by folding over the generated types and calling
    // RegisterTraits<typename Reg::Addr>::read() on the match (one word load, the mock sees it). Fields still go
    // through apply().
    template<typename T>
    struct RegisterTraits;

    template<unsigned A, unsigned WIIZ, unsigned WIIO, typename TRegType, typename TMode>
    struct RegisterTraits<Address<A, WIIZ, WIIO, TRegType, TMode>> {
        static constexpr unsigned address                 = A;
        static constexpr unsigned writeIgnoredIfZeroMask  = WIIZ;
        static constexpr unsigned writeIgnoredIfOneMask   = WIIO;
        static constexpr unsigned allBitsSetMask          = std::numeric_limits<TRegType>::max();
        static constexpr unsigned mustSupplyMask          = TMode::semantics.mustSupply;
        static constexpr bool     readHasSideEffect       = TMode::semantics.readSideEffect;
        static constexpr unsigned writeOnlyNoIdentityMask = TMode::semantics.writeOnlyNoIdentity;
        static constexpr bool     writeOnlyRegister       = TMode::semantics.writeOnlyRegister;
        // Semantics::writeBackActs (default 0: every write-ignored-if-zero bit goes back as read)
        static constexpr unsigned writeBackActsMask = TMode::semantics.writeBackActs & WIIZ;
        // Register::fromReset: a write stores resetFill in the bits it does not name, never reads
        static constexpr bool     fromReset = TMode::semantics.fromReset;
        static constexpr unsigned resetFill = TMode::semantics.resetValue;
        using Mode                          = TMode;
        using RegType                       = TRegType;

        // always inlined: RAM functions reach registers through these (readWhole / writeWhole), and gcc at -Og
        // or with sanitizers would leave a plain inline call in flash
        [[gnu::always_inline]] static TRegType read() { return Mmio<TRegType>::load(A); }

        [[gnu::always_inline]] static void write(TRegType i) { Mmio<TRegType>::store(A, i); }
    };

    template<typename TAddress, unsigned Mask, typename TAccess, typename TFiledType>
    struct RegisterTraits<FieldLocation<TAddress, Mask, TAccess, TFiledType>>
      : RegisterTraits<TAddress> {};

    template<typename TFieldLocation, typename TAction>
    struct RegisterTraits<Action<TFieldLocation, TAction>> : RegisterTraits<TFieldLocation> {};

    // What can be written to a field without changing it. The same table decides the
    // generated registers' masks: svd_converter's identityOf (src/svd/semantics.hpp); keep the two in
    // step. SVD `clear`/`set` stay `none` (the RP2040's SVD uses them for fields whose 0 is a
    // value too); a self-clearing trigger is `oneToSet` in the SVD.
    enum class Identity : unsigned char { none, zero, one };

    constexpr Identity identityOf(AccessType             at,
                                  ModifiedWriteValueType m) {
        using M = ModifiedWriteValueType;
        if(at == AccessType::readOnly) { return Identity::zero; }
        switch(m) {
        case M::oneToClear:
        case M::oneToSet:
        case M::oneToToggle:  return Identity::zero;
        case M::zeroToClear:
        case M::zeroToSet:
        case M::zeroToToggle: return Identity::one;
        default:              return Identity::none;
        }
    }

    template<typename TAccess>
    inline constexpr Identity identityOfAccess = Identity::none;

    template<AccessType AT, ReadActionType RA, ModifiedWriteValueType M>
    inline constexpr Identity identityOfAccess<Access<AT, RA, M>> = identityOf(AT, M);

    // A generated register's write-ignored masks follow from its fields: the zero mask is every
    // bit that is not a field that needs its value (none) or is ignored at one; the one mask
    // the fields ignored at one. svd_converter's computeMasks applies this rule; the generated
    // headers do not call it (a static_assert in a register struct is only evaluated when the
    // register is instantiated) - tests instantiate what they check.
    template<typename TAddress,
             std::size_t N>
    constexpr bool masksMatchFields(std::array<unsigned,
                                               N> const& masks,
                                    std::array<Identity,
                                               N> const& ids) {
        unsigned none = 0;
        unsigned one  = 0;
        for(std::size_t i = 0; i < N; ++i) {
            if(ids[i] == Identity::none) { none |= masks[i]; }
            if(ids[i] == Identity::one) { one |= masks[i]; }
        }
        using A = RegisterTraits<TAddress>;
        return A::writeIgnoredIfOneMask == one
            && A::writeIgnoredIfZeroMask == (A::allBitsSetMask & ~none & ~one);
    }

    namespace Detail {
        template<typename T>
        struct GetMask;

        // from FieldLocations
        template<typename Address, unsigned Mask, typename TAccess, typename ResultType>
        struct GetMask<FieldLocation<Address, Mask, TAccess, ResultType>>
          : std::integral_constant<unsigned, Mask> {};

        // from Action
        template<typename TFieldLocation, typename TAction>
        struct GetMask<Action<TFieldLocation, TAction>> : GetMask<TFieldLocation> {};

    }   // namespace Detail
}}   // namespace Kvasir::Register
