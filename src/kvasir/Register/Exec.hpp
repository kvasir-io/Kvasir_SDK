#pragma once
#include "Types.hpp"
#include "Utility.hpp"
#include "kvasir/Mpl/Algorithm.hpp"
#include "kvasir/Mpl/Types.hpp"
#include "kvasir/Mpl/Utility.hpp"

namespace Kvasir { namespace Register {

    namespace Detail {

        template<typename TRegisterAction>
        struct RegisterExec;

        template<typename TLocation, unsigned ClearMask, unsigned SetMask>
        struct GenericReadMaskOrWrite {
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
                if constexpr(
                  bitsWithFixedValues
                  != allBitsSetMask)   // no sense reading if we are going to clear the whole thing any way
                {
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
            static_assert((Data & (~Mask)) == 0,
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
            static_assert((Data & (~Mask)) == 0,
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
            static_assert((Data & (~Mask)) == 0,
                          "bad mask");
        };
    }   // namespace Detail

    template<typename T, typename U>
    struct ExecuteSeam : Detail::RegisterExec<T> {};
}}   // namespace Kvasir::Register
