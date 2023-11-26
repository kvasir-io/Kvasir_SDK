#pragma once

//#include "Interrupt.hpp"
#include "kvasir/Mpl/Utility.hpp"
//#include "Register/Register.hpp"

namespace Kvasir { namespace Startup {
    template<typename T, typename... Ts>
    struct FirstInitStep {
        static_assert(MPL::AlwaysFalse<T>::value, "You must include a Core");
    };
}}   // namespace Kvasir::Startup
