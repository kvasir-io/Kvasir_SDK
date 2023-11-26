#pragma once

#ifdef __arm__
    #ifdef __clang__
        #include "detail/arm_Clang_atomic.hpp"
    #else
        #include "detail/arm_GCC_atomic.hpp"
    #endif
#else
    #include "detail/host_atomic.hpp"
#endif
