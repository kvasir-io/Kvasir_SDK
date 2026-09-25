#pragma once

// Attribute spellings that differ between the two toolchains, so each compiler gets its nearest
// equivalent instead of an ignored-attribute warning.
//
// A RAM function may run while the flash is not readable, so it carries no check whose handler
// lives in flash (the UB sanitizer's, __stack_chk_fail): one that fired could only lock the core up.
#ifdef __clang__
    #define KVASIR_RAM_FUNC_ATTRIBUTES                                         \
        gnu::section(".data"), gnu::noinline, clang::no_sanitize("undefined"), \
          gnu::no_stack_protector
    // inlined into a RAM function, it brings its instrumentation along: exempt as well
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES                                      \
        gnu::section(".data"), gnu::always_inline, clang::no_sanitize("undefined")
    #define KVASIR_RESETISR_ATTRIBUTES noreturn
    #define KVASIR_ALWAYS_INLINE       clang::always_inline
    // no atexit() registration for the static's destructor
    #define KVASIR_NO_DESTROY                    clang::no_destroy
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW clang::no_sanitize("unsigned-integer-overflow")
#else
    // gcc rejects a section attribute on inline functions and needs long_call for the RAM copy.
    // noipa: under LTO gcc clones (constprop) member, template and inline functions and puts the
    // clone in .text, dropping the section - it keeps it only with noipa (or used).
    #define KVASIR_RAM_FUNC_ATTRIBUTES                                            \
        gnu::section(".data#"), gnu::noinline, gnu::long_call, gnu::noipa,        \
          gnu::no_sanitize("undefined", "bounds-strict"), gnu::no_stack_protector
    #define KVASIR_RAM_FUNC_INLINE_ATTRIBUTES                              \
        gnu::always_inline, gnu::no_sanitize("undefined", "bounds-strict")
    #define KVASIR_RESETISR_ATTRIBUTES noreturn, gnu::naked
    #define KVASIR_ALWAYS_INLINE       gnu::always_inline
    // gcc has no no_destroy; the atexit() registration lands in StartUp.hpp's stub instead
    #define KVASIR_NO_DESTROY
    // gcc has no unsigned-overflow sanitizer; naming it anyway earns a -Wattributes
    #define KVASIR_NO_SANITIZE_UNSIGNED_OVERFLOW
#endif

// The first statement of every KVASIR_RAM_FUNC_ATTRIBUTES function. It costs no instruction: a label
// whose address goes into the non-loaded section kvasir_ram_funcs (linker/common.ld). The label
// travels with the code, so it names where the compiler really put the function, and the post-link
// check (cmake/tools/check_ram_funcs.py) fails the build unless that is RAM and nothing the function
// reaches runs from flash. The check also refuses a RAM function in the image without this mark.
#if defined(__arm__) || defined(__thumb__)
    #define KVASIR_RAM_FUNC_MARK_IN(section)                                                  \
        asm volatile(".pushsection " section ",\"\",%progbits\n.4byte 1f\n.popsection\n1:\n")
#else
    #define KVASIR_RAM_FUNC_MARK_IN(section)
#endif
#define KVASIR_RAM_FUNC_MARK() KVASIR_RAM_FUNC_MARK_IN("kvasir_ram_funcs")
// For a RAM function that calls into flash on purpose, once the flash is readable again (say
// why next to it): it must be in RAM itself, what it calls is not checked.
#define KVASIR_RAM_FUNC_MARK_CALLS_FLASH() KVASIR_RAM_FUNC_MARK_IN("kvasir_ram_funcs_calls_flash")
