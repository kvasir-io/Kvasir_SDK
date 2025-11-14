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
