#pragma once
#include "Diagnostic.hpp"
#include "Types.hpp"
#include "Utility.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

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

        template<typename TLocation, unsigned ClearMask, unsigned SetMask>
        struct GenericReadMaskOrWrite {
            // false when the write covers every bit that is not ignored anyway: one store
            // a write-only register is never read: nothing in it reads back
            static constexpr bool needsRead
              = !GetAddress<TLocation>::writeOnlyRegister
             && ((ClearMask | GetAddress<TLocation>::writeIgnoredIfZeroMask)
                 | (GetAddress<TLocation>::writeIgnoredIfOneMask & ~ClearMask))
                  != GetAddress<TLocation>::allBitsSetMask;

            unsigned operator()(unsigned in = 0) {
                using Address = GetAddress<TLocation>;
                static constexpr auto clearOrZeroIsNoChangeMask
                  = ClearMask | Address::writeIgnoredIfZeroMask;
                static constexpr auto oneIsNoChangeMask
                  = (Address::writeIgnoredIfOneMask
                     & ~ClearMask);   // remove the bits we are working on
                static constexpr auto bitsWithFixedValues
                  = oneIsNoChangeMask | clearOrZeroIsNoChangeMask;
                static constexpr auto     allBitsSetMask = Address::allBitsSetMask;
                decltype(Address::read()) i              = 0;
                // no sense reading if we are going to clear the whole thing any way, nor a register with
                // nothing to read
                if constexpr(bitsWithFixedValues != allBitsSetMask && !Address::writeOnlyRegister) {
                    KVASIR_STATIC_ASSERT(
                      (rmwAllowed<Address, ClearMask>()),
                      (Diagnostic::RmwRefused<Address, ClearMask>),
                      "this write reads the register first (a read-modify-write), which "
                      "the register does not allow: its read has a side effect, or it "
                      "has key bits the write does not supply - write the whole "
                      "register, key included (Register::RmwHazard)");
                    i = Address::read();
                    i &= ~(clearOrZeroIsNoChangeMask);
                }
                i |= SetMask | oneIsNoChangeMask | in;

                Address::write(i);
                return i;
            }
        };

        template<typename TLocation, unsigned ClearMask, unsigned XorMask>
        struct GenericReadMaskXorWrite {
            unsigned operator()(unsigned in = 0) {
                using Address = GetAddress<TLocation>;
                // The target bits (ClearMask) must be written back with their current
                // value xor-ed with the mask: on one-to-toggle hardware the resulting
                // bit value is old ^ written, so writing (old ^ XorMask) forces the
                // field to XorMask. A xor of 0 therefore clears the field (this is what
                // Register::clear on oneToToggle bits relies on). The read is always
                // required because the written value depends on the current bit value.
                static constexpr auto zeroIsNoChangeMask
                  = Address::writeIgnoredIfZeroMask & ~ClearMask;
                static constexpr auto oneIsNoChangeMask
                  = Address::writeIgnoredIfOneMask & ~ClearMask;
                static_assert(
                  !Address::writeOnlyRegister,
                  "a toggle needs the current value, and this register cannot be read (no readable "
                  "field: Register::WriteOnlyRegister)");
                KVASIR_STATIC_ASSERT(
                  (rmwAllowed<Address, ClearMask>()),
                  (Diagnostic::RmwRefused<Address, ClearMask>),
                  "this write reads the register first (a read-modify-write), which the "
                  "register does not allow: its read has a side effect, or it has key "
                  "bits the write does not supply (Register::RmwHazard)");
                decltype(Address::read()) i = Address::read();
                i &= ~zeroIsNoChangeMask;
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
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data> {
            KVASIR_STATIC_ASSERT(((Data & (~Mask)) == 0),
                                 (Diagnostic::BadMask<TAddress,
                                                      Mask,
                                                      Data>),
                                 "bad mask");
        };

        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             WriteRuntimeAndLiteralAction<Data>>>
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data> {
            KVASIR_STATIC_ASSERT(((Data & (~Mask)) == 0),
                                 (Diagnostic::BadMask<TAddress,
                                                      Mask,
                                                      Data>),
                                 "bad mask");
        };

        template<typename TAddress, unsigned Mask, typename Access, typename FieldType>
        struct RegisterExec<
          Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>, WriteAction>>
          : GenericReadMaskOrWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, 0> {};

        template<typename TAddress, unsigned Mask, typename Access, typename FieldType>
        struct RegisterExec<
          Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>, ReadAction>> {
            unsigned operator()(unsigned = 0) { return GetAddress<TAddress>::read(); }
        };

        template<typename TAddress,
                 unsigned Mask,
                 typename Access,
                 typename FieldType,
                 unsigned Data>
        struct RegisterExec<Register::Action<FieldLocation<TAddress, Mask, Access, FieldType>,
                                             XorLiteralAction<Data>>>
          : GenericReadMaskXorWrite<FieldLocation<TAddress, Mask, Access, FieldType>, Mask, Data> {
            KVASIR_STATIC_ASSERT(((Data & (~Mask)) == 0),
                                 (Diagnostic::BadMask<TAddress,
                                                      Mask,
                                                      Data>),
                                 "bad mask");
        };
    }   // namespace Detail

    template<typename T, typename U>
    struct ExecuteSeam : Detail::RegisterExec<T> {};

    namespace Detail {
#if defined(__arm__) && !defined(KVASIR_REGISTER_MOCK)
        // PRIMASK saved, interrupts off, restored: all the Register layer knows about the core
        struct IrqMasked {
            unsigned primask;

            IrqMasked() { asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask) : : "memory"); }

            ~IrqMasked() { asm volatile("msr primask, %0" : : "r"(primask) : "memory"); }

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

            unsigned operator()(unsigned = 0) {
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
