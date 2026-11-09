#pragma once
#include "Diagnostic.hpp"
#include "Types.hpp"
#include "Utility.hpp"

namespace Kvasir { namespace Register {

    namespace Detail {

        template<typename TRegisterAction>
        struct RegisterExec;

        // A read-modify-write that writes ClearMask of the register: allowed unless the read has a
        // side effect, or a key bit or an unclassified write-only bit outside ClearMask would be written
        // back as read.
        template<typename TAddress,
                 unsigned ClearMask>
        constexpr bool rmwAllowed() {
            return !TAddress::readHasSideEffect && (TAddress::mustSupplyMask & ~ClearMask) == 0
                && (TAddress::writeOnlyNoIdentityMask & ~ClearMask) == 0;
        }

        // The read of a read-modify-write that writes ClearMask (the OR and the XOR below): refused where
        // rmwAllowed() says no.
        template<typename Address,
                 unsigned ClearMask>
        [[gnu::always_inline]] inline auto rmwRead() {
            KVASIR_STATIC_ASSERT((rmwAllowed<Address, ClearMask>()),
                                 (Diagnostic::RmwRefused<Address, ClearMask>));
            return Address::read();
        }

        // A literal written into a field has no bit outside the field's mask (the literal, runtime-and-literal and
        // XOR executors below derive from it).
        template<typename TAddress, unsigned Mask, unsigned Data>
        struct LiteralInMask {
            KVASIR_STATIC_ASSERT(((Data & (~Mask)) == 0),
                                 (Diagnostic::BadMask<RegisterTraits<TAddress>,
                                                      Mask,
                                                      Data>));
        };

        template<typename TLocation, unsigned ClearMask, unsigned SetMask>
        struct GenericReadMaskOrWrite {
            // false when the write covers every bit that is not ignored anyway: one store
            // a write-only register is never read: nothing in it reads back
            // Register::fromReset: never read, the unnamed bits are the register's reset value
            static constexpr bool needsRead
              = !RegisterTraits<TLocation>::writeOnlyRegister
             && !RegisterTraits<TLocation>::fromReset
             && ((ClearMask | RegisterTraits<TLocation>::writeIgnoredIfZeroMask)
                 | (RegisterTraits<TLocation>::writeIgnoredIfOneMask & ~ClearMask))
                  != RegisterTraits<TLocation>::allBitsSetMask;

            [[gnu::always_inline]] unsigned operator()(unsigned in = 0) {
                using Address = RegisterTraits<TLocation>;
                static constexpr auto oneIsNoChangeMask
                  = (Address::writeIgnoredIfOneMask
                     & ~ClearMask);   // remove the bits we are working on
                // What the read value loses before the OR: the written bits that end as 0 (a bit the OR sets needs no
                // clearing) and the one-to-* bits outside them, whose 1 read back would act again. Reserved and
                // read-only bits stay as read: a set() is ldr/orr/str, a clear() a bic of the field alone.
                static constexpr auto readClearMask
                  = (ClearMask & ~SetMask) | (Address::writeBackActsMask & ~ClearMask);
                decltype(Address::read()) i = 0;
                // a write-only register is never read, so a key bit the write does not name would go out as 0
                if constexpr(Address::writeOnlyRegister) {
                    KVASIR_STATIC_ASSERT(((Address::mustSupplyMask & ~ClearMask) == 0),
                                         (Diagnostic::KeyNotNamed<Address, ClearMask>));
                }
                if constexpr(needsRead) {
                    i = rmwRead<Address, ClearMask>();
                    if constexpr(readClearMask != 0) { i &= ~readClearMask; }
                }
                i |= SetMask | oneIsNoChangeMask | in;
                if constexpr(Address::fromReset) {
                    static constexpr auto fill = Address::resetFill & ~ClearMask;
                    if constexpr(fill != 0) { i |= fill; }
                }

                Address::write(i);
                return i;
            }
        };

        template<typename TLocation, unsigned ClearMask, unsigned XorMask>
        struct GenericReadMaskXorWrite {
            [[gnu::always_inline]] unsigned operator()(unsigned in = 0) {
                using Address = RegisterTraits<TLocation>;
                // The target bits (ClearMask) must be written back with their current
                // value xor-ed with the mask: on one-to-toggle hardware the resulting
                // bit value is old ^ written, so writing (old ^ XorMask) forces the
                // field to XorMask. A xor of 0 therefore clears the field (this is what
                // Register::clear on oneToToggle bits relies on). The read is always
                // required because the written value depends on the current bit value.
                // What the read value loses: the one-to-* bits outside the field (writeBackActs), as in the OR above.
                static constexpr auto readClearMask = Address::writeBackActsMask & ~ClearMask;
                static constexpr auto oneIsNoChangeMask
                  = Address::writeIgnoredIfOneMask & ~ClearMask;
                static_assert(
                  !Address::writeOnlyRegister,
                  "a toggle needs the current value, and this register cannot be read (no readable "
                  "field: Register::Semantics::writeOnlyRegister)");
                decltype(Address::read()) i = rmwRead<Address, ClearMask>();
                i &= ~readClearMask;
                i |= oneIsNoChangeMask;
                i ^= XorMask | in;
                Address::write(i);
                return i;
            }
        };

        // write literal with read modify write
        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             WriteLiteralAction<Data>>>
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data>
          , LiteralInMask<TAddress, Mask, Data> {};

        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data,
                 unsigned RuntimeMask>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             WriteRuntimeAndLiteralAction<Data, RuntimeMask>>>
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data>
          , LiteralInMask<TAddress, Mask, Data> {};

        template<typename TAddress, unsigned Mask, typename Access, typename FieldType>
        struct RegisterExec<
          Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>, WriteAction>>
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, 0> {};

        template<typename TAddress, unsigned Mask, typename Access, typename FieldType>
        struct RegisterExec<
          Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>, ReadAction>> {
            [[gnu::always_inline]] unsigned operator()(unsigned = 0) {
                return RegisterTraits<TAddress>::read();
            }
        };

        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             XorLiteralAction<Data>>>
          : GenericReadMaskXorWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data>
          , LiteralInMask<TAddress, Mask, Data> {};
    }   // namespace Detail

    // The hook that executes one access of an apply() (Port/Hooks.hpp's table): ExecuteSeam<Tag::User, Action>, named
    // by Apply.hpp through Port::LateT. The default is the plain access (Detail::RegisterExec); a chip specialises it for
    // Kvasir::Tag::User and one access kind on its addresses (chip_rp_common RegisterAlias.hpp: atomic() through the
    // SET/CLR/XOR aliases; chip_stm32f413 chip/OdrToggle.hpp: an ODR toggle through BSRR), in the header its package
    // names in KVASIR_CHIP_REGISTER_SEAM - included at the end of this file, so every TU that reaches apply() sees the
    // chip's access before its first instantiation.
    template<typename T, typename... Ts>
    struct ExecuteSeam : Detail::RegisterExec<Ts...> {};

    // The hook that names the hardware state a register writes, where a chip reaches one state through several
    // registers (Port/Hooks.hpp's table): SharedState<Tag::User, Address> with `static constexpr unsigned value`, the
    // state (the address of its own register, the lowest that writes it - apply() looks for the others from there up;
    // 0, the default: the register is a state of its own) and, optionally, `static consteval void
    // describe(Kvasir::Diagnostic::Text&, unsigned mask)`: the bits and the fix in the chip's words. apply() refuses
    // two writes of one step that reach the same bits of one state through two registers (Apply.hpp,
    // Diagnostic::OneStateTwoRegisters): a step runs by register address, not in the order written, so the address
    // order would pick what the state keeps (chip_atsam_common RegisterSeam.hpp: a SAM PORT pin's level through OUT,
    // OUTCLR, OUTSET and OUTTGL; chip_rp_common RegisterAlias.hpp: an RP pin's through SIO GPIO_OUT, _SET, _CLR and
    // _XOR). Specialised in the KVASIR_CHIP_REGISTER_SEAM header, as ExecuteSeam.
    template<typename T, typename... Ts>
    struct SharedState {
        static constexpr unsigned value = 0;
    };

    namespace Detail {
#if defined(__arm__) && !defined(KVASIR_REGISTER_MOCK)
        // PRIMASK saved, interrupts off, restored: Core::IrqGuard (core/InterruptMask.hpp) by hand, because the
        // register layer may not include the core (tools/layers.toml). The same restore: msr, then isb - a lowered
        // PRIMASK is visible only after a context synchronization event (InterruptMask.hpp has the citations).
        struct IrqMasked {
            unsigned primask;

            [[gnu::always_inline]] IrqMasked() {
                asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask) : : "memory");
            }

            [[gnu::always_inline]] ~IrqMasked() {
                asm volatile("msr primask, %0\n isb" : : "r"(primask) : "memory");
            }

            IrqMasked(IrqMasked const&)            = delete;
            IrqMasked& operator=(IrqMasked const&) = delete;
        };
#else
        struct IrqMasked {};
#endif

        // Register::atomic's default: the read-modify-write with interrupts masked; a write that
        // needs no read is one store already and gets no mask
        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             AtomicWriteLiteralAction<Data>>> {
            using Rmw = GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>,
                                               Mask,
                                               Data>;

            [[gnu::always_inline]] unsigned operator()(unsigned = 0) {
                if constexpr(Rmw::needsRead) {
                    [[maybe_unused]] IrqMasked const guard{};
                    return Rmw{}();
                } else {
                    return Rmw{}();
                }
            }
        };
    }   // namespace Detail
}}   // namespace Kvasir::Register

// The chip's ExecuteSeam and SharedState specialisations (above): the chip package's kvasir_chip(DEFINITIONS) names the
// header. It includes this file first (#pragma once ends the cycle), so it may be included on its own too. Host tests
// and chips without a seam have no macro and get the plain access, every register a state of its own.
#ifdef KVASIR_CHIP_REGISTER_SEAM
    #include KVASIR_CHIP_REGISTER_SEAM
#endif
