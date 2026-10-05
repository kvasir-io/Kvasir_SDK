#pragma once
#include "AtomicFactories.hpp"
#include "Diagnostic.hpp"
#include "Types.hpp"
#include "Utility.hpp"

namespace Kvasir { namespace Register {
    namespace Detail {
        using namespace MPL;

        // factory for write literal
        template<typename TLocation, unsigned Value>
        struct Write;

        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename TFieldType,
                 unsigned Value>
        struct Write<FieldLocation<TAddress, Mask, Access, TFieldType>, Value>
          : Action<FieldLocation<TAddress, Mask, Access, TFieldType>,
                   WriteLiteralAction<(Value << positionOfFirstSetBit(Mask))>> {
            KVASIR_STATIC_ASSERT(
              (IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access,
                                        TFieldType>>::value),
              (Diagnostic::NotWritable<TAddress,
                                       Mask>),
              "Access violation: the FieldLocation provided is not marked as writable");
            KVASIR_STATIC_ASSERT(
              (literalFits(Mask,
                           Value)),
              (Diagnostic::LiteralTooWide<TAddress,
                                          Mask,
                                          Value>),
              "literal does not fit the field: bits above the field's width would be "
              "shifted out and silently lost");
        };

        template<typename TLocation, unsigned Value>
        using WriteT = typename Write<TLocation, Value>::type;

        template<typename TLocation>
        struct Set;

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct Set<FieldLocation<TAddress, Mask, Access, TFieldType>>
          : Action<FieldLocation<TAddress, Mask, Access, TFieldType>, WriteLiteralAction<Mask>> {
            KVASIR_STATIC_ASSERT(
              (onlyOneBitSet(Mask)),
              (Diagnostic::NotSingleBit<TAddress,
                                        Mask,
                                        's'>),
              "Register::set only works on single bits. Use Register::write to write values to "
              "wider bit fields");
            KVASIR_STATIC_ASSERT(
              (IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access,
                                        TFieldType>>::value),
              (Diagnostic::NotWritable<TAddress,
                                       Mask>),
              "Access violation: the FieldLocation provided is not marked as writable");
        };

        template<typename TLocation>
        using SetT = typename Set<TLocation>::type;

        template<typename TLocation>
        struct Clear;

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct Clear<FieldLocation<TAddress, Mask, Access, TFieldType>>
          : Action<FieldLocation<TAddress, Mask, Access, TFieldType>, WriteLiteralAction<0>> {
            KVASIR_STATIC_ASSERT(
              (onlyOneBitSet(Mask)),
              (Diagnostic::NotSingleBit<TAddress,
                                        Mask,
                                        'c'>),
              "Register::clear only works on single bits. Use Register::write to write values to "
              "wider bit fields");
            KVASIR_STATIC_ASSERT(
              (IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access,
                                        TFieldType>>::value),
              (Diagnostic::NotWritable<TAddress,
                                       Mask>),
              "Access violation: the FieldLocation provided is not marked as writable");
        };

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
            KVASIR_STATIC_ASSERT(
              false,
              (Diagnostic::ClearOnFlag<TAddress,
                                       Mask>),
              "Register::clear writes a 0, which a write-one-to-clear or "
              "write-one-to-set field ignores: use Register::reset to clear a flag");
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
                   XorLiteralAction<0>> {
            static_assert(accessWritable(AT),
                          "Access violation: the FieldLocation provided is not marked as writable");
            static_assert(accessReadable(AT),
                          "Register::clear on a write-only toggle field: it cannot be read back, "
                          "and a written 0 toggles nothing");
        };

        template<typename TLocation>
        using ClearT = typename Clear<TLocation>::type;

        // toggle(): a plain bit is read and written back inverted (GenericReadMaskXorWrite with
        // the bit's mask); on a one-to-toggle field the hardware does it, a single store of the
        // bit. (XorLiteralAction<V> on a one-to-toggle field would *force* it to V instead - which
        // is what clear() uses there.)
        template<typename TLocation>
        struct Toggle;

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct Toggle<FieldLocation<TAddress, Mask, Access, TFieldType>>
          : Action<FieldLocation<TAddress, Mask, Access, TFieldType>, XorLiteralAction<Mask>> {
            KVASIR_STATIC_ASSERT((onlyOneBitSet(Mask)),
                                 (Diagnostic::NotSingleBit<TAddress,
                                                           Mask,
                                                           't'>),
                                 "Register::toggle only works on single bits");
            KVASIR_STATIC_ASSERT(
              (IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access,
                                        TFieldType>>::value
               && IsReadable<FieldLocation<TAddress,
                                           Mask,
                                           Access,
                                           TFieldType>>::value),
              (Diagnostic::ToggleNeedsRead<TAddress,
                                           Mask>),
              "Register::toggle reads the bit and writes it back inverted: the field "
              "must be readable and writable");
        };

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
                   WriteLiteralAction<Mask>> {
            static_assert(accessWritable(AT),
                          "Access violation: the FieldLocation provided is not marked as writable");
        };

        template<typename TLocation>
        using ToggleT = typename Toggle<TLocation>::type;

        template<typename TLocation>
        struct ResetImpl;

        template<typename TAddress, unsigned Mask, typename Access, typename TFieldType>
        struct ResetImpl<FieldLocation<TAddress, Mask, Access, TFieldType>> {
            // every bit of the field written as 1: a multi-bit flag field is cleared whole
            using type
              = Action<FieldLocation<TAddress, Mask, Access, TFieldType>, WriteLiteralAction<Mask>>;
            KVASIR_STATIC_ASSERT(
              (IsWritable<FieldLocation<TAddress,
                                        Mask,
                                        Access,
                                        TFieldType>>::value),
              (Diagnostic::NotWritable<TAddress,
                                       Mask>),
              "Access violation: the FieldLocation provided is not marked as writable");
            static_assert(Detail::IsSetToClear<FieldLocation<TAddress,
                                                             Mask,
                                                             Access,
                                                             TFieldType>>::value,
                          "Access violation: Register::reset only works on set to clear bits");
        };

        template<typename TLocation>
        using Reset = typename ResetImpl<TLocation>::type;

    }   // namespace Detail

    // Action factories which turn a FieldLocation into an Action
    template<typename T>
    constexpr inline MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                                    Action<T,
                                           ReadAction>>
    read(T) {
        static_assert(Detail::IsReadable<T>::value,
                      "Access violation: the FieldLocation provided is not marked as readable");
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(read(T{}),
                                 read(U{}),
                                 read(Ts{})...)) read(T,
                                                      U,
                                                      Ts...) {
        return {};
    }

    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                             Detail::SetT<T>>
    set(T) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(set(T{}),
                                 set(U{}),
                                 set(Ts{})...)) set(T,
                                                    U,
                                                    Ts...) {
        return {};
    }

    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                             Detail::ClearT<T>>
    clear(T) {
        return {};
    }

    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                             Detail::ToggleT<T>>
    toggle(T) {
        return {};
    }

    // constrained, unlike set/clear's: Kvasir::Io has a variadic toggle() of pins
    template<typename T,
             typename U,
             typename... Ts>
        requires(Detail::IsFieldLocation<T>::value && Detail::IsFieldLocation<U>::value
                 && (Detail::IsFieldLocation<Ts>::value && ...))
    constexpr decltype(MPL::list(toggle(T{}),
                                 toggle(U{}),
                                 toggle(Ts{})...)) toggle(T,
                                                          U,
                                                          Ts...) {
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(clear(T{}),
                                 clear(U{}),
                                 clear(Ts{})...)) clear(T,
                                                        U,
                                                        Ts...) {
        return {};
    }

    template<typename T>
    constexpr MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                             Detail::Reset<T>>
    reset(T) {
        static_assert(Detail::IsSetToClear<T>::value,
                      "Access violation: Register::reset only works on set to clear bits");
        return {};
    }

    template<typename T,
             typename U,
             typename... Ts>
    constexpr decltype(MPL::list(reset(T{}),
                                 reset(U{}),
                                 reset(Ts{})...)) reset(T,
                                                        U,
                                                        Ts...) {
        return {};
    }

    // Write of runtime value
    // T must be bit location or function will be removed from overload set
    template<typename T>
    constexpr inline MPL::EnableIfT<Detail::IsFieldLocation<T>::value,
                                    Action<T,
                                           WriteAction>>
    write(T,
          Detail::GetFieldTypeT<T> in) {
        static_assert(Detail::IsWritable<T>::value,
                      "Access violation: The FieldLocation provided is not marked as writable");
        constexpr auto mask  = Detail::GetMask<T>::value;
        constexpr auto start = Detail::maskStartsAt(mask);
        return Action<T, WriteAction>{mask & (unsigned(in) << start)};
    }

    // compile time value
    // T must be bit location or function will be removed from overload set
    // U mst be compile time value or function will be removed from overload set
    template<typename T,
             typename U>
    constexpr inline MPL::EnableIfT<(Detail::IsFieldLocation<T>::value && MPL::IsValue<U>::value),
                                    Detail::WriteT<T,
                                                   Detail::ValueToUnsigned<U>::value>>
    write(T,
          U) {
        static_assert(
          Detail::WriteLocationAndCompileTimeValueTypeAreSame<T, U>::value,
          "type mismatch: the FieldLocation field type and the compile time Value type must be the "
          "same");
        return {};
    }

    // compile time field value
    // T must be a field value or function will be removed from overload set
    template<typename T,
             typename T::DataType V>
    constexpr inline decltype(write(T{},
                                    MPL::Value<typename T::DataType,
                                               V>{})) write(FieldValue<T,
                                                                       V>) {
        return {};
    }

    // variadic compile time field values
    template<typename T,
             typename U,
             typename... Ts>
    constexpr brigand::list<decltype(write(std::declval<T>())),
                            decltype(write(std::declval<U>())),
                            decltype(write(std::declval<Ts>()))...>
    write(T,
          U,
          Ts...) {
        return {};
    }
}}   // namespace Kvasir::Register
