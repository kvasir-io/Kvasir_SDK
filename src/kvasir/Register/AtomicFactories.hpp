#pragma once
#include "Diagnostic.hpp"
#include "Utility.hpp"

// Register::atomic(set(A::x), clear(A::y), write(B::f, value<3>())): literal writes that must not
// lose a concurrent update of another field of the same register - from an ISR, or from the other
// core. Atomic actions of one register merge (one access); an atomic and a plain write to the same
// register stay two accesses, literal or runtime (Apply.hpp's merge table). What it costs is the chip's: on the RP chips a store to the register's
// set/clear/xor alias (chip_rp_common/RegisterAlias.hpp), elsewhere a read-modify-write with
// interrupts masked (Exec.hpp; not atomic against another core - no such chip is supported).
// It protects the OTHER fields of the register; two writers of the same field still race.
//
// A one-to-set trigger (a self-clearing bit whose 0 does nothing, `oneToSet` in the SVD: PIO CTRL.SM_RESTART) takes
// set() only - on the RP chips one store to the SET alias, pico-sdk's hw_set_bits (RP2040 datasheet 2.1.2, RP2350
// 2.1.3: an alias write acts on its mask's bits), elsewhere the masked read-modify-write, which writes the register's
// other triggers 0 (Register::Semantics::writeBackActs). A clear or a 0 of a trigger means nothing and is refused,
// as is any atomic write of a flag (one-to-clear, one-to-toggle) or a read-only field: an alias write would clear or
// toggle it - reset() clears a flag.
namespace Kvasir { namespace Register {
    namespace Detail {
        template<typename TAccess>
        inline constexpr bool oneToSetAccess = false;

        template<AccessType AT, ReadActionType RA>
        inline constexpr bool oneToSetAccess<Access<AT, RA, ModifiedWriteValueType::oneToSet>>
          = AT != AccessType::readOnly;

        template<typename T>
        struct MakeAtomic {
            static_assert(false,
                          "Register::atomic takes literal writes (set, clear, write of a "
                          "compile-time value)");
        };

        template<typename L, unsigned V>
        struct MakeAtomic<Action<L, WriteLiteralAction<V>>> {
            // An alias write of a flag's bit clears or toggles it (RP2350): reset() is the way to clear a flag. A
            // one-to-set trigger is fine with every bit 1 (set()).
            static constexpr bool trigger = oneToSetAccess<typename L::Access>;
            KVASIR_STATIC_ASSERT((identityOfAccess<typename L::Access> != Identity::zero
                                  || (trigger && V == L::mask)),
                                 (Diagnostic::AtomicOnFlag<RegisterTraits<L>,
                                                           L::mask,
                                                           trigger>));
            using type = Action<L, AtomicWriteLiteralAction<V>>;
        };

        template<typename... Ts>
        struct MakeAtomic<brigand::list<Ts...>> {
            using type = brigand::list<typename MakeAtomic<Ts>::type...>;
        };
    }   // namespace Detail

    template<typename... Ts>
    [[gnu::always_inline]] constexpr auto atomic(Ts...) {
        return brigand::flatten<brigand::list<typename Detail::MakeAtomic<Ts>::type...>>{};
    }
}}   // namespace Kvasir::Register
