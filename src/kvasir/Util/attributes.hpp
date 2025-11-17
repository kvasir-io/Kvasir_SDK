#pragma once

#ifdef __clang__
    #define KVASIR_RAM_FUNC_ATTRIBUTES        gnu::section(".data"), gnu::noinline
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES gnu::section(".data"), gnu::always_inline
    #define KVASIR_RESETISR_ATTRIBUTES        noreturn
#else
    #define KVASIR_RAM_FUNC_ATTRIBUTES        gnu::section(".data#"), gnu::noinline, gnu::long_call
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES gnu::section(".data"), gnu::always_inline
    #define KVASIR_RESETISR_ATTRIBUTES        noreturn, gnu::naked
#endif
