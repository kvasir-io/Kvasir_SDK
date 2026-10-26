#pragma once
#include "Diagnostic.hpp"   // Register/Diagnostic.hpp: FieldNotRead
#include "kvasir/Mpl/Algorithm.hpp"

#include <bit>
#include <cstddef>

namespace Kvasir { namespace Register {

    struct SequencePoint {
        using type = SequencePoint;
    };

    static constexpr SequencePoint sequencePoint{};

    // A register's write facts, one value per register (svd_converter emits the members that differ from the
    // default, as designated initialisers in this order):
    //  - mustSupply: key bits every write names - they read back something other than what must be written (SCB
    //    AIRCR.VECTKEY reads 0xFA05, needs 0x05FA); an SVD field whose writeConstraint is a range of one value;
    //  - readSideEffect: a read pops or clears something (I2C IC_DATA_CMD pops the RX FIFO), so only whole-register
    //    writes; an SVD field with a readAction;
    //  - writeOnlyNoIdentity: write-only bits that a read cannot give back and that nobody classified
    //    (svd_converter --write-only-mask): a partial write must name them, like a key;
    //  - writeOnlyRegister: no readable field (svd_converter --write-only-registers=derived): a partial write never
    //    reads it - there is nothing to keep - and writes the bits it does not name as 0, as a FULLREGISTER write of
    //    the same value would; a key in it must still be named (Exec.hpp);
    //  - writeBackActs: which write-ignored-if-zero bits a read-modify-write must write as 0 - the one-to-* fields,
    //    whose value read back and written again would act (clear a pending flag, fire a trigger). The rest of that
    //    mask - reserved and read-only bits - is written back as read, so a field write is one OR or BIC of the
    //    field's own bits. 0 (the default): nothing acts, every bit goes back as read; a hand-written Address with a
    //    write-one-to-* bit (or a bit it wants written 0) in its write-ignored-if-zero mask states it here;
    //  - fromReset, resetValue: never set by svd_converter - Register::fromReset<R>() (Register.hpp) marks the
    //    Address of its actions: a write is one store, no read, of the named fields over R's reset value
    //    (resetValue) in every bit it does not name.
    struct Semantics {
        unsigned mustSupply          = 0;
        bool     readSideEffect      = false;
        unsigned writeOnlyNoIdentity = 0;
        bool     writeOnlyRegister   = false;
        unsigned writeBackActs       = 0;
        bool     fromReset           = false;
        unsigned resetValue          = 0;
    };

    // An Address's 5th argument: the write facts and, for compile errors only (Register/Diagnostic.hpp), the
    // generated register struct itself (an incomplete type there, which a template argument may be). Every register
    // with a key, a read side effect or a write-only register names itself; the others only with svd_converter's
    // NAMED_REGISTERS option (off by default, it lengthens every Address type).
    template<Semantics S = Semantics{}, typename TRegister = void>
    struct Mode {
        static constexpr Semantics semantics = S;
        using Register                       = TRegister;
    };

    namespace Detail {
        // The Address/FieldLocation sanity checks as predicates: the static_asserts below call them, the tests test
        // them.
        constexpr bool maskFitsRegister(unsigned    mask,
                                        std::size_t registerBytes) {
            return mask != 0 && static_cast<std::size_t>(std::bit_width(mask)) <= registerBytes * 8;
        }

        constexpr bool addressAligned(unsigned    address,
                                      std::size_t registerBytes) {
            return address % registerBytes == 0;
        }
    }   // namespace Detail

    template<unsigned A,
             unsigned WriteIgnoredIfZeroMask = 0,
             unsigned WriteIgnoredIfOneMask  = 0,
             typename TRegType               = unsigned,
             typename TMode                  = Mode<>>
    struct Address {
        using type    = Address<A, WriteIgnoredIfZeroMask, WriteIgnoredIfOneMask, TRegType, TMode>;
        using RegType = TRegType;
        using Mode    = TMode;
        static constexpr unsigned value = A;
        static_assert(Detail::addressAligned(A,
                                             sizeof(TRegType)),
                      "register address is not aligned to the register's width");
    };

    // write a compile time known value
    template<unsigned I>
    struct WriteLiteralAction {
        static constexpr unsigned value = I;
    };

    // write a compile time and runtime time known value: a literal merged with runtime writes of
    // the same register (Apply.hpp's merge table); RuntimeMask the bits the runtime values cover
    template<unsigned I, unsigned RuntimeMask>
    struct WriteRuntimeAndLiteralAction {
        static constexpr unsigned value = I;
        unsigned                  value_;
    };

    // A literal write that must not lose a concurrent update of another field of the register
    // (Register::atomic, AtomicFactories.hpp). How is the chip's business (the ExecuteSeam hook,
    // Exec.hpp: the RP set/clear/xor aliases); the default masks interrupts around the read-modify-write.
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
        [[gnu::always_inline]] constexpr Action(Ts... args) : TAction{args...} {}

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
        static constexpr auto mask = TMask;
        static_assert(TMask != 0,
                      "a FieldLocation needs at least one bit");
        static_assert(Detail::maskFitsRegister(TMask,
                                               sizeof(typename TAddress::RegType)),
                      "the field's mask has bits beyond the register's width");
    };

    namespace Detail {
        // the first bit of a field's mask: the shift between the field's value and the register word
        constexpr unsigned maskStartsAt(unsigned mask) { return unsigned(std::countr_zero(mask)); }
    }   // namespace Detail

    template<typename TFieldLocation, typename TFieldLocation::DataType Value>
    struct FieldValue {
        using type = FieldValue<TFieldLocation, Value>;

        operator typename TFieldLocation::DataType() const { return Value; }
    };
    template<typename TAddresses, typename TFieldLocation>
    struct FieldTuple;   // see below for implementation in specialization

    namespace Detail {
        template<typename Object, typename TFieldLocation>
        struct GetFieldLocationIndex;

        template<typename TA, typename TLocations, typename TFieldLocation>
        struct GetFieldLocationIndex<FieldTuple<TA, TLocations>, TFieldLocation>
          : MPL::Find<TLocations, TFieldLocation> {};

    }   // namespace Detail

    // The result of an apply() with reads: Is is the slot of each read field (in argument order),
    // one slot per read access - fields of one register read in one step share it.
    template<uint32_t... Is,
             typename... TAs,
             unsigned... Masks,
             typename... TAccesss,
             typename... TRs>
    struct FieldTuple<brigand::list<brigand::uint32_t<Is>...>,
                      brigand::list<FieldLocation<TAs, Masks, TAccesss, TRs>...>> {
        static constexpr unsigned numSlots = [] {
            unsigned n = 0;
            ((n = Is + 1 > n ? Is + 1 : n), ...);
            return n;
        }();

        // a built-in array, not std::array: std::array's operator[] is not always_inline, and with less inlining
        // (clang -Oz with UBSan) it stays out of line in flash, called from a RAM function's register read
        // (check_ram_funcs). Every index below is a compile-time constant. Test: tests/ram_funcs, CASE 8.
        unsigned value_[numSlots];

        // result[field] names the field's read: refused for a field read in two steps
        template<typename T>
        static constexpr bool readInOneStep() {
            constexpr bool same[]
              = {std::is_same_v<T, FieldLocation<TAs, Masks, TAccesss, TRs>>...};
            constexpr uint32_t slots[] = {Is...};
            bool               found   = false;
            uint32_t           slot    = 0;
            for(std::size_t i = 0; i < sizeof...(Is); ++i) {
                if(!same[i]) { continue; }
                if(found && slots[i] != slot) { return false; }
                found = true;
                slot  = slots[i];
            }
            return true;
        }

        template<std::size_t Index>
        [[nodiscard,
          gnu::always_inline]] brigand::at_c<brigand::list<TRs...>,
                                             Index>
        get() const {
            static constexpr unsigned index
              = brigand::at_c<brigand::list<brigand::uint32_t<Is>...>, Index>::value;
            using ResultType = brigand::at_c<brigand::list<TRs...>, Index>;
            static constexpr unsigned mask
              = brigand::at_c<brigand::list<brigand::uint32_t<Masks>...>, Index>::value;
            static constexpr unsigned shift = Detail::maskStartsAt(mask);
            unsigned                  r     = (value_[index] & mask) >> shift;
            return ResultType(r);
        }

        // A field this apply() did not read is a named compile error (Diagnostic::FieldNotRead), not an overload
        // failure; the else branch only keeps that one error from cascading.
        template<typename T>
        [[gnu::always_inline]] auto operator[](T) const {
            static constexpr int index = Detail::GetFieldLocationIndex<FieldTuple, T>::value;
            KVASIR_STATIC_ASSERT(index >= 0, (Diagnostic::FieldNotRead<T>));
            static_assert(readInOneStep<T>(),
                          "this field is read in more than one step of the apply(): result[field] "
                          "cannot say which read is meant - use get<I>() with the read's position");
            if constexpr(index >= 0) {
                return get<static_cast<std::size_t>(index)>();
            } else {
                return typename T::DataType{};
            }
        }

        struct DoNotUse {
            template<typename T>
            explicit DoNotUse(T) {}
        };

        // implicitly convertible to the field type only if there is just one field
        using ConvertableTo = std::
          conditional_t<(sizeof...(TRs) == 1), brigand::at_c<brigand::list<TRs...>, 0>, DoNotUse>;

        // const: it only reads value_
        [[gnu::always_inline]]
        operator ConvertableTo() const {
            // brigand::at_c, not `Masks...[0]`: the clang 23 static analyzer (clang-tidy's clang-analyzer-*) crashes
            // on an NTTP pack index used as a value
            static constexpr unsigned mask
              = brigand::at_c<brigand::list<brigand::uint32_t<Masks>...>, 0>::value;
            static constexpr unsigned shift = Detail::maskStartsAt(mask);
            return ConvertableTo((value_[0] & mask) >> shift);
        }
    };

    template<>
    struct FieldTuple<brigand::list<>, brigand::list<>> {};

    template<std::size_t I,
             typename TFieldTuple>
    [[gnu::always_inline]] inline auto get(TFieldTuple o) -> decltype(o.template get<I>()) {
        return o.template get<I>();
    }

    // only for FieldTuples, so it never joins an unrelated two-argument get() overload set; a field not read is
    // operator[]'s named error
    template<typename T,
             typename TA,
             typename TL>
    [[gnu::always_inline]] inline auto get(T              t,
                                           FieldTuple<TA,
                                                      TL> o) {
        return o[t];
    }

    template<typename TFieldTuple,
             typename TLocation,
             typename TLocation::DataType Value>
    [[gnu::always_inline]] inline bool operator==(TFieldTuple const& f,
                                                  FieldValue<TLocation,
                                                             Value> const) {
        return get(TLocation{}, f) == Value;
    }
}}   // namespace Kvasir::Register
