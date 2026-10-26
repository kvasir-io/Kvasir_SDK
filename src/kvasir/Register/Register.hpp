#pragma once
#include "Apply.hpp"
#include "Factories.hpp"
#include "Fields.hpp"
#include "Mmio.hpp"
#include "Types.hpp"
#include "kvasir/Mpl/Types.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir { namespace Register {
    // this function produces an MPL::List just like MPL::list, however putting
    // one here allows adl to find it without the user having to write out the
    // whole namespace. Making the list take at least one parameter should
    // prevent ambiguity in a case where MPL::list is also an overload candidate
    template<typename T,
             typename... Ts>
    constexpr brigand::list<T,
                            Ts...>
    list(T,
         Ts...) {
        return brigand::list<T, Ts...>{};
    }

    // factory for compile time values; always inlined: a RAM function names them (tests/ram_funcs CASE 8)
    template<std::uint32_t I>
    [[gnu::always_inline]] constexpr std::integral_constant<std::uint32_t,
                                                            I>
    value() {
        return {};
    }

    template<typename T,
             T I>
    [[gnu::always_inline]] constexpr std::integral_constant<T,
                                                            I>
    value() {
        return {};
    }

    // bit helpers
    template<typename Address, int Position, typename TFieldType = bool>
    using RWBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), ReadWriteAccess, TFieldType>;
    template<typename Address, int Position, typename TFieldType = bool>
    using WOBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), WriteOnlyAccess, TFieldType>;

    namespace Detail {
        // R::overrideDefaults takes writes only: the default writes it adds are those of the fields no action names,
        // so a read, toggle or atomic write of a field would drop that field's default write
        template<typename TAction>
        struct OverrideIsWrite;

        template<typename L, typename TKind>
        struct OverrideIsWrite<Action<L, TKind>> {
            static constexpr Kind kind = KindOf<TKind>::kind;
            KVASIR_STATIC_ASSERT((kind == Kind::literal || kind == Kind::runtime
                                  || kind == Kind::literalAndRuntime),
                                 (Diagnostic::OverrideDefaultsNotAWrite < RegisterTraits<L>,
                                  L::mask,
                                  kind == Kind::read     ? Diagnostic::NotAWrite::read
                                  : kind == Kind::toggle ? Diagnostic::NotAWrite::toggle
                                  : kind == Kind::atomic ? Diagnostic::NotAWrite::atomic
                                                         : Diagnostic::NotAWrite::other >));
        };

        // R::overrideDefaults' work: which of the register's default writes (Defaults, flattened) the given actions
        // leave out
        template<typename D>
        struct OverrideDefaults;

        template<typename... Defaults>
        struct OverrideDefaults<brigand::list<Defaults...>> {
            template<typename T>
            static constexpr bool isInDefault() {
                return (std::is_same_v<typename T::location, typename Defaults::location> || ...);
            }

            template<typename D,
                     typename... TTs>
            static constexpr auto getDefaultIfNotInTs() {
                using DLoc = typename D::location;
                if constexpr((std::is_same_v<typename TTs::location, DLoc> || ...)) {
                    return brigand::list<>{};
                } else {
                    return D{};
                }
            }

            template<typename T>
            struct DefaultValues;

            template<typename... Ts>
            struct DefaultValues<brigand::list<Ts...>> {
                static constexpr auto value() {
                    if constexpr(
                      requires { (Defaults::isAction, ...); } && requires { (Ts::isAction, ...); })
                    {
                        static_assert(((sizeof(OverrideIsWrite<typename Ts::type>) != 0) && ...));
                        static_assert(
                          (isInDefault<Ts>() && ...),
                          "overrideDefaults: an action names a field that is not one of the "
                          "register's writable fields");
                        return list(getDefaultIfNotInTs<Defaults, Ts...>()...);
                    } else {
                        static_assert(false,
                                      "not an action");   // C++26 (P2593), as AtomicFactories.hpp
                    }
                }
            };

            // the default writes of every field the actions Ts do not name
            template<typename... Ts>
            using Remaining = brigand::flatten<
              decltype(DefaultValues<brigand::flatten<brigand::list<Ts...>>>::value())>;
        };

        // an action that carries a runtime value (write(field, x)): it exists only as a value, never in a type list
        template<typename T>
        inline constexpr bool carriesRuntimeValue = requires(T const& t) { t.value_; };
    }   // namespace Detail

    // R::overrideDefaults(actions...): the actions plus a write of its reset value to every other writable field of R
    // (Defaults = Register::DefaultValues<R>, Views.hpp). With literal actions only it is a type list, as any list of
    // actions (a Startup list may hold it); with a runtime value among them it is a Defaulted bundle (Apply.hpp),
    // which apply() takes alone: apply(R::overrideDefaults(write(R::f, x), set(R::g))) runs exactly
    // apply(write(R::f, x), set(R::g), <the remaining defaults>). Always inlined: a RAM function may call it.
    template<typename Defaults,
             typename... Ts>
    [[gnu::always_inline]] constexpr auto overrideDefaults(Ts const&... ts) {
        using Remaining =
          typename Detail::OverrideDefaults<brigand::flatten<Defaults>>::template Remaining<Ts...>;
        if constexpr((Detail::carriesRuntimeValue<Ts> || ...)) {
            return Defaulted<Remaining, Ts...>{ts...};
        } else {
            return brigand::append<brigand::list<Ts...>, Remaining>{};
        }
    }

    namespace Detail {
        consteval Semantics fromResetSemantics(Semantics s,
                                               unsigned  resetValue) {
            s.fromReset  = true;
            s.resetValue = resetValue;
            return s;
        }

        template<typename R, typename T>
        struct FromReset {
            static_assert(
              false,
              "Register::fromReset<R> takes literal writes of R's fields (set, clear, write of a "
              "compile-time value)");
        };

        template<typename R,
                 unsigned A,
                 unsigned Z,
                 unsigned O,
                 typename T,
                 typename M,
                 unsigned Mask,
                 typename Acc,
                 typename FT,
                 unsigned V>
        struct FromReset<
          R,
          Action<FieldLocation<Address<A, Z, O, T, M>, Mask, Acc, FT>, WriteLiteralAction<V>>> {
            static_assert(A == R::Addr::value,
                          "Register::fromReset<R>: every action writes a field of R (one register "
                          "per fromReset)");
            using type = Action<
              FieldLocation<
                Address<A,
                        Z,
                        O,
                        T,
                        Mode<fromResetSemantics(M::semantics, static_cast<unsigned>(R::resetValue)),
                             typename M::Register>>,
                Mask,
                Acc,
                FT>,
              WriteLiteralAction<V>>;
        };

        template<typename R, typename... Ts>
        struct FromReset<R, brigand::list<Ts...>> {
            using type = brigand::list<typename FromReset<R, Ts>::type...>;
        };
    }   // namespace Detail

    // Register::fromReset<R>(set(R::a), write(R::b, value<3>())): literal writes of fields of R whose access is ONE
    // store, no read, with every bit no action names at R's reset value. Actions of one register from several
    // fromReset<R>() - several Startup entries of one merged init step (SAM EIC: each EXTINT line's sense and filter
    // in the shared CONFIGn) - merge into that one store; the fields nobody names are R's reset, whatever the register
    // held before (a bootloader's leftovers included). A plain write of R in the same step is a compile error (the
    // fill would undo it). A type list like any list of literal actions: a Startup list may hold it.
    template<typename R,
             typename... Ts>
    [[gnu::always_inline]] constexpr auto fromReset(Ts...) {
        return brigand::flatten<brigand::list<typename Detail::FromReset<R, Ts>::type...>>{};
    }

    // A register's whole word as one plain load or store, always inlined, where the value really is the whole word: a
    // data, FIFO or counter register, a snapshot, a register array indexed in a loop. Fields go through apply() - in a
    // RAM function too, its whole chain is always inlined (gcc -Og and sanitize included). Pass the FULLREGISTER:
    //
    //     auto const status = readWhole(QMI::DIRECT_CSR::FULLREGISTER);
    //     writeWhole(QMI::DIRECT_TX::FULLREGISTER, word);
    //
    // A store writes the word as given: no read-modify-write, no key added.
    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] inline auto readWhole(T) {
        KVASIR_STATIC_ASSERT((Detail::IsReadable<T>::value),
                             (Diagnostic::NotReadable<RegisterTraits<T>, T::mask>));
        KVASIR_STATIC_ASSERT((T::mask == RegisterTraits<T>::allBitsSetMask),
                             (Diagnostic::NotFullRegister<RegisterTraits<T>, T::mask>));
        return RegisterTraits<T>::read();
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] inline void writeWhole(T,
                                                  decltype(RegisterTraits<T>::read()) v) {
        KVASIR_STATIC_ASSERT((Detail::IsWritable<T>::value),
                             (Diagnostic::NotWritable<RegisterTraits<T>, T::mask>));
        KVASIR_STATIC_ASSERT((T::mask == RegisterTraits<T>::allBitsSetMask),
                             (Diagnostic::NotFullRegister<RegisterTraits<T>, T::mask>));
        RegisterTraits<T>::write(v);
    }

    // A field's value out of a word readWhole() returned (a RAM function reads the register once, then names its
    // fields): (word & mask) >> the field's first bit. A one-bit field also has its mask as `field.mask`.
    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr std::uint32_t extract(T,
                                                           std::uint32_t word) {
        constexpr auto mask = Detail::GetMask<T>::value;
        if constexpr(std::has_single_bit(mask)) {
            return (word & mask) != 0
                   ? 1U
                   : 0U;   // the same 0/1, but `extract(f, w) != 0` stays one tst
        } else {
            return (word & mask) >> Detail::maskStartsAt(mask);
        }
    }

    // The other way round: a field's bits in a word for writeWhole() - (value << the field's first bit) & mask.
    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] constexpr std::uint32_t deposit(T,
                                                           std::uint32_t value) {
        constexpr auto mask = Detail::GetMask<T>::value;
        return (value << Detail::maskStartsAt(mask)) & mask;
    }

    // One bit of a multi-bit field, its index known at compile time: a state machine's bit of PIO CTRL.SM_ENABLE, an
    // endpoint's of USB EP_ABORT, where the SVD has one field for all of them. The same register and write semantics,
    // a one-bit field of its own, so set/clear/reset/read work on it as on any field:
    //
    //     apply(set(bit<Sm>(Pio::CTRL::sm_enable)));
    //
    // T is the one-bit field's value type, std::uint32_t by default; `bit<Pin, bool>(SIO::GPIO_IN::gpio_in)` reads a
    // pin as a bool.
    template<unsigned N,
             typename T = std::uint32_t,
             typename TAddress,
             unsigned Mask,
             typename TAccess,
             typename TFieldType>
    consteval auto bit(FieldLocation<TAddress,
                                     Mask,
                                     TAccess,
                                     TFieldType>) {
        static_assert(
          ((Mask >> Detail::maskStartsAt(Mask)) & ((Mask >> Detail::maskStartsAt(Mask)) + 1U)) == 0,
          "bit<N>: the field's bits must be contiguous");
        static_assert(N < static_cast<unsigned>(std::popcount(Mask)),
                      "bit<N>: N is outside the field");
        return FieldLocation<TAddress, (1U << (Detail::maskStartsAt(Mask) + N)), TAccess, T>{};
    }

    namespace Detail {
        // the fields of R (its generated `fields`) that hold bit Bit: how many, and the index of the first
        template<typename R>
        consteval std::size_t fieldsHolding(unsigned bit) {
            std::size_t n = 0;
            if(bit < 32U) {
                for(auto const m : fieldMasks<R>) { n += ((m >> bit) & 1U); }
            }
            return n;
        }

        template<typename R>
        consteval std::size_t firstFieldHolding(unsigned bit) {
            std::size_t i = 0;
            while(((fieldMasks<R>[i] >> bit) & 1U) == 0) { ++i; }
            return i;
        }

        template<typename R, unsigned Bit, typename L>
        struct FieldAtImpl;

        template<typename R, unsigned Bit, typename... Fs>
        struct FieldAtImpl<R, Bit, Fields<Fs...>> {
            static constexpr std::size_t count = fieldsHolding<R>(Bit);
            KVASIR_STATIC_ASSERT(Bit < 32U && count == 1,
                                 (Diagnostic::FieldAtBit<R,
                                                         Bit,
                                                         count>));
            // void after the named error (no field to name)
            using type = std::remove_cvref_t<
              brigand::at_c<brigand::list<Fs..., void>,
                            count == 1 ? firstFieldHolding<R>(Bit) : sizeof...(Fs)>>;
        };
    }   // namespace Detail

    // The generated field of register R that holds bit Bit - for an SVD that names one field per pin or per channel
    // (STM32 GPIOx_MODER.MODERn, BSRR.BSn, AFRL.AFRLn), so code indexes the real field, with its access and value
    // type, instead of building a mask: `write(fieldAt<2 * Pin>(GPIO::MODER{}), value<1>())`. A bit no field holds
    // (reserved) or that two fields share is a named error.
    template<typename R, unsigned Bit>
    using FieldAt = typename Detail::FieldAtImpl<R, Bit, typename R::fields>::type;

    template<unsigned Bit,
             typename R>
    [[gnu::always_inline]] consteval auto fieldAt(R) {
        return FieldAt<R, Bit>{};
    }

    // One byte or halfword lane of a register as a store of that width, for a register whose interface is defined by
    // the access size (the SAM D21's GCLK GENCTRL: an 8-bit write of its ID byte selects the generator to read back).
    // Lane counts in units of Lane from the register's address.
    template<typename Lane,
             unsigned Index,
             typename T>
        requires(Detail::IsFieldLocation<T>::value && std::is_unsigned_v<Lane>)
    [[gnu::always_inline]] inline void writeLane(T,
                                                 Lane v) {
        using Reg = decltype(RegisterTraits<T>::read());
        static_assert(sizeof(Lane) < sizeof(Reg) && (Index + 1) * sizeof(Lane) <= sizeof(Reg),
                      "writeLane: the lane must be narrower than the register and inside it");
        KVASIR_STATIC_ASSERT((Detail::IsWritable<T>::value),
                             (Diagnostic::NotWritable<RegisterTraits<T>, T::mask>));
        RegisterTraits<
          Address<RegisterTraits<T>::address + Index * sizeof(Lane), 0, 0, Lane>>::write(v);
    }

    // A register array (an SVD dim: Element<0> .. Element<Count - 1>, evenly spaced) indexed at run time with one
    // plain load or store, no jump table. The stride comes from the first two elements; naming Element<Count - 1>
    // checks Count against the generated bound. The caller keeps the index below Count.
    //
    //     using Instr = Register::Array<PioRegs::template INSTR_MEM, 32>;
    //     Instr::writeWhole(offset + i, instruction);
    template<template<unsigned> class Element, unsigned Count>
    struct Array {
    private:
        template<unsigned I>
        using Location = std::remove_cvref_t<decltype(Element<I>::FULLREGISTER)>;
        using First    = Location<0>;

    public:
        static_assert(Count > 0,
                      "Register::Array of no element");
        using RegType = decltype(RegisterTraits<First>::read());

        static constexpr unsigned size = Count;
        static constexpr unsigned base = RegisterTraits<First>::address;
        static constexpr unsigned stride
          = Count > 1 ? RegisterTraits<Location<(Count > 1 ? 1 : 0)>>::address - base
                      : sizeof(RegType);
        static_assert(RegisterTraits<Location<Count - 1>>::address == base + (Count - 1) * stride,
                      "Register::Array: the elements are not evenly spaced");

        [[gnu::always_inline]] static RegType readWhole(unsigned i) {
            KVASIR_STATIC_ASSERT((Detail::IsReadable<First>::value),
                                 (Diagnostic::NotReadable<RegisterTraits<First>, First::mask>));
            return Mmio<RegType>::load(std::uintptr_t{base + i * stride});
        }

        [[gnu::always_inline]] static void writeWhole(unsigned i,
                                                      RegType  v) {
            KVASIR_STATIC_ASSERT((Detail::IsWritable<First>::value),
                                 (Diagnostic::NotWritable<RegisterTraits<First>, First::mask>));
            Mmio<RegType>::store(std::uintptr_t{base + i * stride}, v);
        }
    };
}}   // namespace Kvasir::Register
