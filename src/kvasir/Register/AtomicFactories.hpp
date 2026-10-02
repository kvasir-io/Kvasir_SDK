#pragma once
#include "Utility.hpp"

// Register::atomic(set(A::x), clear(A::y), write(B::f, value<3>())): literal writes that must not
// lose a concurrent update of another field of the same register - from an ISR, or from the other
// core. Atomic actions of one register merge (one access); an atomic and a plain write to the same
// register stay two accesses. What it costs is the chip's: on the RP chips a store to the register's
// set/clear/xor alias (chip_rp_common/RegisterAlias.hpp), elsewhere a read-modify-write with
// interrupts masked (Exec.hpp; not atomic against another core - no such chip is supported).
// It protects the OTHER fields of the register; two writers of the same field still race.
namespace Kvasir { namespace Register {
    namespace Detail {
        template<typename T>
        struct MakeAtomic {
            static_assert(false,
                          "Register::atomic takes literal writes (set, clear, write of a "
                          "compile-time value)");
        };

        template<typename L, unsigned V>
        struct MakeAtomic<Action<L, WriteLiteralAction<V>>> {
            // An alias write of a one-to-* field's bit clears or toggles it (RP2350), a set of it
            // does nothing: reset() is the way to clear a flag.
            static_assert(
              identityOfAccess<typename L::Access> != Identity::zero,
              "Register::atomic on a one-to-clear/one-to-set/one-to-toggle or read-only "
              "field: use reset() for a flag");
            using type = Action<L, AtomicWriteLiteralAction<V>>;
        };

        template<typename... Ts>
        struct MakeAtomic<brigand::list<Ts...>> {
            using type = brigand::list<typename MakeAtomic<Ts>::type...>;
        };
    }   // namespace Detail

    template<typename... Ts>
    constexpr auto atomic(Ts...) {
        return brigand::flatten<brigand::list<typename Detail::MakeAtomic<Ts>::type...>>{};
    }
}}   // namespace Kvasir::Register
