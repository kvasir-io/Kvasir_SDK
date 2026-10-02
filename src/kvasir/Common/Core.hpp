#pragma once

//#include "Interrupt.hpp"
#include "kvasir/Mpl/Utility.hpp"

#include <cstdint>

//#include "Register/Register.hpp"

namespace Kvasir { namespace Startup {
    template<typename T, typename... Ts>
    struct FirstInitStep {
        static_assert(MPL::AlwaysFalse<T>::value,
                      "You must include a Core");
    };

    // The chip's source of per-boot randomness for the stack guard (__stack_chk_guard), read once
    // per boot by core 0 between initMemory() and the global constructors. The default has none
    // and returns the old constant: the canary still catches a stray write, it just is not secret.
    // Specialised by the chip layer for Tag::User.
    template<typename T, typename... Ts>
    struct StackGuardEntropy {
        [[gnu::always_inline]] std::uint32_t operator()() const { return 0xdeadc0deU; }
    };

    // RAM the chip's linker script adds beside .data/.bss (chip_rp2040: the SRAM4/5 scratch
    // banks): copied and zeroed by core 0 right after initMemory(), before the global
    // constructors. The default has none. Specialised by the chip layer for Tag::User.
    template<typename T, typename... Ts>
    struct ExtraMemoryInit {
        [[gnu::always_inline]] void operator()() const {}
    };

    // The secondary-core counterpart of FirstInitStep: what a core other than the boot core
    // needs done for itself. Specialised by the chip layer for Tag::User with
    //   static constexpr std::uint32_t cpacrEnable;   // ORed into CPACR by the entry trampoline
    //   void operator()();                             // anything else, from C++
    template<typename T, typename... Ts>
    struct SecondaryCoreInit {
        static_assert(MPL::AlwaysFalse<T>::value,
                      "You must include a Core that supports a secondary core");
    };
}}   // namespace Kvasir::Startup
