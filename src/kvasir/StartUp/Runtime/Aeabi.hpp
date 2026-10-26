#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end:
// the AEABI memory helpers (clang + libgcc + llvm-libc only) and the EHABI personality stubs.
// Strong definitions that must exist once per image.
// No includes of its own on purpose: it relies on what StartUp.hpp includes before it (Runtime/Panic.hpp, uc_log,
// LinkerSymbols.hpp, Common/Interrupt.hpp, <cassert> <cerrno> <cstdlib> <cstring> <new> <string_view>), and a second
// <cassert> would re-enter libc's unguarded assert.h (re-declares uc_log::log_assert, -Wredundant-decls).
#ifdef __arm__

    #if defined(__clang__) && defined(KVASIR_COMPILER_RT_LIBGCC) && defined(LIBC_NAMESPACE)
// clang lowers struct copies and memset to the AEABI helpers, which neither libgcc nor llvm-libc
// provides. Argument order matters: __aeabi_memset takes (dest, n, value), the reverse of memset.
extern "C" {
[[gnu::used]] inline void __aeabi_memcpy(void*       d,
                                         void const* s,
                                         std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy4(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memcpy8(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memcpy(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove(void*       d,
                                          void const* s,
                                          std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove4(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memmove8(void*       d,
                                           void const* s,
                                           std::size_t n) {
    std::memmove(d, s, n);
}

[[gnu::used]] inline void __aeabi_memset(void*       d,
                                         std::size_t n,
                                         int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset4(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memset8(void*       d,
                                          std::size_t n,
                                          int         v) {
    std::memset(d, v, n);
}

[[gnu::used]] inline void __aeabi_memclr(void*       d,
                                         std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr4(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}

[[gnu::used]] inline void __aeabi_memclr8(void*       d,
                                          std::size_t n) {
    std::memset(d, 0, n);
}
}
    #endif

extern "C" {
// EHABI personality routines. Nothing here unwinds, but the sanitizers make clang emit .ARM.exidx
// entries whose personality reference would pull libgcc's whole unwinder out of the archive;
// defining the routines keeps it out.
[[gnu::used]] inline void __aeabi_unwind_cpp_pr0() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr1() {}

[[gnu::used]] inline void __aeabi_unwind_cpp_pr2() {}
}

#endif
