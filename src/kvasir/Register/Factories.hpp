#pragma once
#include "AtomicFactories.hpp"
#include "Diagnostic.hpp"
#include "Types.hpp"
#include "Utility.hpp"
#include "kvasir/Mpl/Types.hpp"

namespace Kvasir { namespace Register {
    namespace Detail {
        // The factories' checks, one KVASIR_STATIC_ASSERT each, as bases (as Exec.hpp's LiteralInMask): a factory
        // derives from its Action and the checks it needs, and instantiating it runs them. A check is no static data
        // member: one of a class template is never evaluated unless used. No check defines `type`: a factory's
        // `::type` is its Action (Kvasir::Io's pin factories derive from Set/Clear and read it).
        template<typename L>
        struct Writable {
            KVASIR_STATIC_ASSERT((IsWritable<L>::value),
                                 (Diagnostic::NotWritable<RegisterTraits<L>,
                                                          L::mask>));
        };

        // toggle(): the bit is read and written back inverted
        template<typename L>
        struct ReadableAndWritable {
            KVASIR_STATIC_ASSERT((IsWritable<L>::value && IsReadable<L>::value),
                                 (Diagnostic::ToggleNeedsRead<RegisterTraits<L>,
                                                              L::mask>));
        };

        template<typename L, Diagnostic::Verb V>
        struct SingleBit {
            KVASIR_STATIC_ASSERT((std::has_single_bit(L::mask)),
                                 (Diagnostic::NotSingleBit<RegisterTraits<L>,
                                                           L::mask,
                                                           V>));
        };

        template<typename L, unsigned Value>
        struct Fits {
            KVASIR_STATIC_ASSERT((literalFits(L::mask,
                                              Value)),
                                 (Diagnostic::LiteralTooWide<RegisterTraits<L>,
                                                             L::mask,
                                                             Value>));
        };

        template<typename L>
        struct SetToClear {
            KVASIR_STATIC_ASSERT((IsSetToClear<L>::value),
                                 (Diagnostic::NotSetToClear<RegisterTraits<L>,
                                                            L::mask>));
        };

        template<typename L, unsigned Value>
        struct Write
          : Action<L, WriteLiteralAction<(Value << maskStartsAt(L::mask))>>
          , Writable<L>
          , Fits<L, Value> {};

        template<typename L>
        struct Set
          : Action<L, WriteLiteralAction<L::mask>>
          , SingleBit<L, Diagnostic::Verb::set>
          , Writable<L> {};

        template<typename L>
        struct Clear
          : Action<L, WriteLiteralAction<0>>
          , SingleBit<L, Diagnostic::Verb::clear>
          , Writable<L> {};

        // clear() writes a 0, which a write-one-to-clear or write-one-to-set field ignores: the call
        // would compile and do nothing. A flag is cleared with reset().
        template<typename TAddress,
                 unsigned               Mask,
                 AccessType             AT,
                 ReadActionType         RAT,
                 ModifiedWriteValueType M,
                 typename TFieldType>
            requires(M == ModifiedWriteValueType::oneToClear
                     || M == ModifiedWriteValueType::oneToSet)
        struct Clear<FieldLocation<TAddress, Mask, Access<AT, RAT, M>, TFieldType>> {
            KVASIR_STATIC_ASSERT(false,
                                 (Diagnostic::ClearOnFlag<RegisterTraits<TAddress>,
                                                          Mask>));
        };

        // special case for clearing toggle bits. Writing the value back will clear these, therefore using a xor of 0
        // will clear. Only on a readable register: a write-only toggle register (the RP SIO *_XOR
        // aliases) cannot be read, and writing it a 0 does nothing.
        template<typename TAddress,
                 unsigned       Mask,
                 AccessType     AT,
                 ReadActionType RAT,
                 typename TFieldType>
        struct Clear<FieldLocation<TAddress,
                                   Mask,
                                   Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                   TFieldType>>
          : Action<FieldLocation<TAddress,
                                 Mask,
                                 Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                 TFieldType>,
                   XorLiteralAction<0>>
          , Writable<FieldLocation<TAddress,
                                   Mask,
                                   Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                   TFieldType>> {
            KVASIR_STATIC_ASSERT((accessReadable(AT)),
                                 (Diagnostic::WriteOnlyToggleClear<RegisterTraits<TAddress>,
                                                                   Mask>));
        };

        // toggle(): a plain bit is read and written back inverted (GenericReadMaskXorWrite with
        // the bit's mask); on a one-to-toggle field the hardware does it, a single store of the
        // bit. (XorLiteralAction<V> on a one-to-toggle field would *force* it to V instead - which
        // is what clear() uses there.)
        template<typename L>
        struct Toggle
          : Action<L, XorLiteralAction<L::mask>>
          , SingleBit<L, Diagnostic::Verb::toggle>
          , ReadableAndWritable<L> {};

        template<typename TAddress,
                 unsigned       Mask,
                 AccessType     AT,
                 ReadActionType RAT,
                 typename TFieldType>
        struct Toggle<FieldLocation<TAddress,
                                    Mask,
                                    Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                    TFieldType>>
          : Action<FieldLocation<TAddress,
                                 Mask,
                                 Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                 TFieldType>,
                   WriteLiteralAction<Mask>>
          , Writable<FieldLocation<TAddress,
                                   Mask,
                                   Access<AT, RAT, ModifiedWriteValueType::oneToToggle>,
                                   TFieldType>> {};

        // every bit of the field written as 1: a multi-bit flag field is cleared whole
        template<typename L>
        struct Reset
          : Action<L, WriteLiteralAction<L::mask>>
          , Writable<L>
          , SetToClear<L> {};

        template<typename T>
        struct IsFieldValue : std::false_type {};

        template<typename L, typename L::DataType V>
        struct IsFieldValue<FieldValue<L, V>> : std::true_type {};
    }   // namespace Detail

    // The action factories: a FieldLocation becomes an Action (the factory's checks run), several become a list of
    // them - one shape for every verb, the list of what each argument alone gives. They take fields only (pins have
    // their own factories in Kvasir::Io).
    //
    // read() and the runtime write() check in their body, as they always did: a list names the single factory's
    // type only (decltype), so a list of reads checks nothing - Register::applyFields reads every field of a
    // register, write-only ones too.
    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr Action<T,
                                            ReadAction>
    read(T) {
        KVASIR_STATIC_ASSERT((Detail::IsReadable<T>::value),
                             (Diagnostic::NotReadable<RegisterTraits<T>, T::mask>));
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(read(Ts{}))...> read(Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr typename Detail::Set<T>::type set(T) {
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(set(Ts{}))...> set(Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr typename Detail::Clear<T>::type clear(T) {
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(clear(Ts{}))...> clear(Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr typename Detail::Toggle<T>::type toggle(T) {
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(toggle(Ts{}))...> toggle(Ts...) {
        return {};
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr typename Detail::Reset<T>::type reset(T) {
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldLocation<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(reset(Ts{}))...> reset(Ts...) {
        return {};
    }

    // a runtime value
    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr Action<T,
                                            WriteAction>
    write(T,
          Detail::GetFieldTypeT<T> in) {
        KVASIR_STATIC_ASSERT((Detail::IsWritable<T>::value),
                             (Diagnostic::NotWritable<RegisterTraits<T>, T::mask>));
        constexpr auto mask  = Detail::GetMask<T>::value;
        constexpr auto start = Detail::maskStartsAt(mask);
        return Action<T, WriteAction>{mask & (unsigned(in) << start)};
    }

    // a compile-time value of the field's type: value<FieldType, V>() = std::integral_constant<FieldType, V>. Every
    // MPL::IntegralConstant reaches this overload (a tag too), and the return type is substitution-safe for each of
    // them: a constant this overload refused would convert into the runtime one (integral_constant's operator
    // value_type) and become a load-modify-store. The type is checked in the body, as read()'s access: a list spelled
    // with decltype(write(f, value<V>())) takes a value of another type (the SAM SERCOM CTRLA/BAUD configs write enum
    // fields with value<0>()).
    template<typename T,
             typename U>
        requires(Detail::IsFieldLocation<T>::value && MPL::IntegralConstant<U>)
    [[gnu::always_inline]] constexpr typename Detail::Write<T,
                                                            static_cast<unsigned>(U::value)>::type
    write(T,
          U) {
        KVASIR_STATIC_ASSERT((Detail::WriteLocationAndCompileTimeValueTypeAreSame<T, U>::value),
                             (Diagnostic::ValueTypeMismatch<RegisterTraits<T>, T::mask>));
        return {};
    }

    // a field value (a generated enum value)
    template<typename T,
             typename T::DataType V>
    [[gnu::always_inline]] constexpr decltype(write(T{},
                                                    std::integral_constant<typename T::DataType,
                                                                           V>{}))
      write(FieldValue<T,
                       V>) {
        return {};
    }

    template<typename... Ts>
        requires(sizeof...(Ts) > 1 && (Detail::IsFieldValue<Ts>::value && ...))
    [[gnu::always_inline]] constexpr brigand::list<decltype(write(Ts{}))...> write(Ts...) {
        return {};
    }
}}   // namespace Kvasir::Register
