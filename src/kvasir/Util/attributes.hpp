#pragma once

// Attribute spellings that differ between the two toolchains, so each compiler gets its nearest
// equivalent instead of an ignored-attribute warning.
#ifdef __clang__
    #define KVASIR_RAM_FUNC_ATTRIBUTES        gnu::section(".data"), gnu::noinline
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES gnu::section(".data"), gnu::always_inline
    #define KVASIR_RESETISR_ATTRIBUTES        noreturn
    #define KVASIR_ALWAYS_INLINE              clang::always_inline
    // no atexit() registration for the static's destructor
    #define KVASIR_NO_DESTROY                    clang::no_destroy
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW clang::no_sanitize("unsigned-integer-overflow")
#else
    // gcc rejects a section attribute on inline functions and needs long_call for the RAM copy
    #define KVASIR_RAM_FUNC_ATTRIBUTES        gnu::section(".data#"), gnu::noinline, gnu::long_call
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES gnu::always_inline
    #define KVASIR_RESETISR_ATTRIBUTES        noreturn, gnu::naked
    #define KVASIR_ALWAYS_INLINE              gnu::always_inline
    // gcc has no no_destroy; the atexit() registration lands in StartUp.hpp's stub instead
    #define KVASIR_NO_DESTROY
    // gcc has no unsigned-overflow sanitizer; naming it anyway earns a -Wattributes
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW
#endif
