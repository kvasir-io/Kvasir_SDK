#pragma once

//#include "Interrupt.hpp"
#include "kvasir/Mpl/Utility.hpp"

//#include "Register/Register.hpp"

namespace Kvasir { namespace Startup {
    template<typename T, typename... Ts>
    struct FirstInitStep {
        static_assert(MPL::AlwaysFalse<T>::value,
                      "You must include a Core");
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
