#pragma once
#include "Apply.hpp"
#include "Factories.hpp"
#include "Types.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

#include <bit>
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

    // factory for compile time values
    template<std::uint32_t I>
    constexpr MPL::Value<std::uint32_t,
                         I>
    value() {
        return MPL::Value<std::uint32_t, I>{};
    }

    template<typename T,
             T I>
    constexpr MPL::Value<T,
                         I>
    value() {
        return MPL::Value<T, I>{};
    }

    // bit helpers
    template<typename Address, int Position, typename TFieldType = bool>
    using RWBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), ReadWriteAccess, TFieldType>;
    template<typename Address, int Position, typename TFieldType = bool>
    using ROBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), ReadOnlyAccess, TFieldType>;
    template<typename Address, int Position, typename TFieldType = bool>
    using WOBitLocT
      = FieldLocation<Address, (1U << unsigned(Position)), WriteOnlyAccess, TFieldType>;

    // bit field helpers
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using RWFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), ReadWriteAccess, TFieldType>;
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using ROFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), ReadOnlyAccess, TFieldType>;
    template<typename Address, int HighestBit, int LowestBit, typename TFieldType = unsigned>
    using WOFieldLocT
      = FieldLocation<Address, maskFromRange(HighestBit, LowestBit), WriteOnlyAccess, TFieldType>;

    template<typename D>
    struct overrideDefaults_impl;

    template<typename... Defaults>
    struct overrideDefaults_impl<brigand::list<Defaults...>> {
        template<typename... T>
        struct AlwaysFalse {
            static constexpr bool value = false;
        };

        template<typename T,
                 typename... Ds>
        static constexpr bool isInDefault() {
            return (std::is_same_v<typename T::location, typename Ds::location> || ...);
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
                    static_assert((isInDefault<Ts, Defaults...>() && ...), "not in default");
                    return list(getDefaultIfNotInTs<Defaults, Ts...>()...);
                } else {
                    static_assert(AlwaysFalse<Defaults...>::value, "not an action");
                }
            }
        };

        template<typename... Ts,
                 typename... Vs>
        static constexpr auto makeList(Ts const&... ts,
                                       brigand::list<Vs...>) {
            return list(Ts{ts}..., Vs{}...);
        }

        template<typename... Ts>
        static constexpr auto value(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Ts...>>;
            using FlattenedDefaults
              = brigand::flatten<decltype(DefaultValues<FlattenedActions>::value())>;
            return makeList<Ts...>(ts..., FlattenedDefaults{});
        }

        template<typename... Ts>
        static constexpr auto exec(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Ts...>>;
            using FlattenedDefaults
              = brigand::flatten<decltype(DefaultValues<FlattenedActions>::value())>;
            apply(ts..., FlattenedDefaults{});
        }
    };

    template<typename D>
    struct overrideDefaults;

    template<typename... Defaults>
    struct overrideDefaults<brigand::list<Defaults...>> {
        template<typename... Ts>
        static constexpr auto value(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Defaults...>>;
            return overrideDefaults_impl<FlattenedActions>::value(ts...);
        }
    };

    template<typename D>
    struct overrideDefaultsRuntime;

    template<typename... Defaults>
    struct overrideDefaultsRuntime<brigand::list<Defaults...>> {
        template<typename... Ts>
        static constexpr void exec(Ts const&... ts) {
            using FlattenedActions = brigand::flatten<brigand::list<Defaults...>>;
            return overrideDefaults_impl<FlattenedActions>::exec(ts...);
        }
    };

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
        static_assert(Detail::IsReadable<T>::value, "readWhole: the register is not readable");
        static_assert(Detail::GetMask<T>::value == Detail::GetAddress<T>::allBitsSetMask,
                      "readWhole takes the register's FULLREGISTER");
        return Detail::GetAddress<T>::read();
    }

    template<typename T>
        requires(Detail::IsFieldLocation<T>::value)
    [[gnu::always_inline]] inline void writeWhole(T,
                                                  decltype(Detail::GetAddress<T>::read()) v) {
        static_assert(Detail::IsWritable<T>::value, "writeWhole: the register is not writable");
        static_assert(Detail::GetMask<T>::value == Detail::GetAddress<T>::allBitsSetMask,
                      "writeWhole takes the register's FULLREGISTER");
        Detail::GetAddress<T>::write(v);
    }

    // A field's value out of a word readWhole() returned (a RAM function reads the register once, then names its
    // fields): (word & mask) >> the field's first bit. A one-bit field also has its mask as `field.Mask`.
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
    template<unsigned N,
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
        return FieldLocation<TAddress,
                             (1U << (Detail::maskStartsAt(Mask) + N)),
                             TAccess,
                             std::uint32_t>{};
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
        using Reg = decltype(Detail::GetAddress<T>::read());
        static_assert(sizeof(Lane) < sizeof(Reg) && (Index + 1) * sizeof(Lane) <= sizeof(Reg),
                      "writeLane: the lane must be narrower than the register and inside it");
        static_assert(Detail::IsWritable<T>::value, "writeLane: the register is not writable");
        Detail::GetAddress<
          Address<Detail::GetAddress<T>::value + Index * sizeof(Lane), 0, 0, Lane>>::write(v);
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
        using RegType = decltype(Detail::GetAddress<First>::read());

        static constexpr unsigned size = Count;
        static constexpr unsigned base = Detail::GetAddress<First>::value;
        static constexpr unsigned stride
          = Count > 1 ? Detail::GetAddress<Location<(Count > 1 ? 1 : 0)>>::value - base
                      : sizeof(RegType);
        static_assert(Detail::GetAddress<Location<Count - 1>>::value == base + (Count - 1) * stride,
                      "Register::Array: the elements are not evenly spaced");

        [[gnu::always_inline]] static RegType readWhole(unsigned i) {
            static_assert(Detail::IsReadable<First>::value,
                          "Register::Array::readWhole: the registers are not readable");
#ifdef KVASIR_REGISTER_MOCK
            return ::Kvasir::Test::readAt<RegType>(base + i * stride);
#else
            return *reinterpret_cast<RegType volatile*>(std::uintptr_t{base + i * stride});
#endif
        }

        [[gnu::always_inline]] static void writeWhole(unsigned i,
                                                      RegType  v) {
            static_assert(Detail::IsWritable<First>::value,
                          "Register::Array::writeWhole: the registers are not writable");
#ifdef KVASIR_REGISTER_MOCK
            ::Kvasir::Test::writeAt<RegType>(base + i * stride, v);
#else
            *reinterpret_cast<RegType volatile*>(std::uintptr_t{base + i * stride}) = v;
#endif
        }
    };
}}   // namespace Kvasir::Register
