#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end:
// operator new / delete (without a heap, and with newlib's), std::terminate and libstdc++'s std::__throw_* helpers.
// Strong definitions that must exist once per image.
// No includes of its own on purpose: it relies on what StartUp.hpp includes before it (Runtime/Panic.hpp, uc_log,
// LinkerSymbols.hpp, Common/Interrupt.hpp, <cassert> <cerrno> <cstdlib> <cstring> <new> <string_view>), and a second
// <cassert> would re-enter libc's unguarded assert.h (re-declares uc_log::log_assert, -Wredundant-decls).
#ifndef KVASIR_HEAP
void operator delete(void*) noexcept {}

void operator delete(void*,
                     std::size_t) noexcept {}

void operator delete[](void*) noexcept {}

void operator delete[](void*,
                       std::size_t) noexcept {}

    // keep libc++'s new.cpp (and its malloc) out of the link; reaching one is a bug
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wmissing-noreturn"

void* operator new(std::size_t) {
    #ifdef __arm__
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::allocation,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "operator new without a heap (HEAP_SIZE)");
    #else
    assert(false);
    __builtin_trap();
    #endif
}

void* operator new[](std::size_t) {
    #ifdef __arm__
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::allocation,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "operator new[] without a heap (HEAP_SIZE)");
    #else
    assert(false);
    __builtin_trap();
    #endif
}

    #pragma GCC diagnostic pop
#endif

#ifdef __arm__
// std::terminate is the abort panic in every image: libc++ calls it on its unreachable paths (a noexcept function left
// by an exception, std::visit of a valueless variant, emio's and other libraries' error ends) and the freestanding
// libc++ build defines none; libstdc++'s own (eh_terminate.o) would bring the verbose handler, the demangler and stdio.
// The default terminate handler is std::abort, so the cause is abort's. Not gnu::used: an image that never calls it
// keeps no byte of it (a library archive's reference is resolved before LTO drops anything). No [[noreturn]] here:
// <exception> declares it noreturn already, and clang refuses the attribute on a redeclaration when the first one
// spells it `__attribute__((__noreturn__))` (libstdc++: the clang + newlib trees).
// NOLINTNEXTLINE(bugprone-std-namespace-modification) // the one definition the freestanding libc++ lacks
void std::terminate() noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "std::terminate() called");
}
#endif

#if defined(KVASIR_HEAP) && defined(__NEWLIB__)
// A gcc build with a heap. libstdc++'s own operator new throws std::bad_alloc when malloc returns null, and that one
// throw links the unwinder (__exidx_start / __exidx_end, which no script here defines), the verbose terminate handler
// and newlib's stdio behind it ("_write is not implemented and will always fail", ...: gcc's speed
// variants). These replace it: straight to newlib's malloc, and an exhausted heap is the allocation
// panic, as an allocation without a heap is above.
namespace Kvasir::Startup::Detail {
[[gnu::always_inline]] inline void* allocateOrPanic(void* block,
                                                    void* caller) {
    if(block == nullptr) {
        KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::allocation,
                               ::Kvasir::Panic::Detail::sitePc(caller),
                               0,
                               "operator new: the heap (HEAP_SIZE) is exhausted");
    }
    return block;
}
}   // namespace Kvasir::Startup::Detail

void* operator new(std::size_t size) {
    return Kvasir::Startup::Detail::allocateOrPanic(std::malloc(size == 0 ? 1 : size),
                                                    __builtin_return_address(0));
}

void* operator new[](std::size_t size) {
    return Kvasir::Startup::Detail::allocateOrPanic(std::malloc(size == 0 ? 1 : size),
                                                    __builtin_return_address(0));
}

void operator delete(void* block) noexcept { std::free(block); }

void operator delete(void* block,
                     std::size_t) noexcept {
    std::free(block);
}

void operator delete[](void* block) noexcept { std::free(block); }

void operator delete[](void* block,
                       std::size_t) noexcept {
    std::free(block);
}

// new (std::nothrow): libstdc++'s wraps the throwing one in a try block (the personality routine, the unwinder)
void* operator new(std::size_t size,
                   std::nothrow_t const&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size,
                     std::nothrow_t const&) noexcept {
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* block,
                     std::nothrow_t const&) noexcept {
    std::free(block);
}

void operator delete[](void* block,
                       std::nothrow_t const&) noexcept {
    std::free(block);
}
#endif

#if defined(__NEWLIB__) && defined(__GLIBCXX__) && defined(__arm__)
// libstdc++'s error paths (a container asked for too much, vector::at out of range, bad_alloc) end in its
// std::__throw_* helpers, all fifteen in one object of the library (functexcept.o). In the full library - gcc's speed
// variants - they really throw: the unwinder, the type-info tables, the verbose terminate handler with its demangler
// and newlib's stdio come with that one object, and the image does not link (__exidx_start; "_write is not implemented
// and will always fail"). Defined here, the linker never takes that object: each helper
// logs what would have been thrown and raises the abort panic at the caller. Under their mangled names, as the
// __sync_* shims are, so no declaration here has to match the library's. All fifteen, or the linker takes the object
// for the missing one and reports the others as defined twice (the list is the same in libstdc++ 16.2's full and
// nano libraries, v6-M and v8-M).
namespace Kvasir::Startup::Detail {
[[gnu::noreturn,
  gnu::always_inline]] inline void
libstdcxxThrow(std::string_view exception,
               char const*      what,
               void*            caller) {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           ::Kvasir::Panic::Detail::sitePc(caller),
                           0,
                           "libstdc++ would throw std::{}: {}",
                           exception,
                           std::string_view{what == nullptr ? "" : what});
}

    #define KVASIR_LIBSTDCXX_THROW(function, mangled, exception)             \
        [[gnu::noreturn, gnu::used]] void function() __asm__(mangled);       \
        [[gnu::noreturn, gnu::used]] void function() {                       \
            libstdcxxThrow(exception, nullptr, __builtin_return_address(0)); \
        }
    #define KVASIR_LIBSTDCXX_THROW_WHAT(function, mangled, exception)                  \
        [[gnu::noreturn, gnu::used]] void function(char const* what) __asm__(mangled); \
        [[gnu::noreturn, gnu::used]] void function(char const* what) {                 \
            libstdcxxThrow(exception, what, __builtin_return_address(0));              \
        }
KVASIR_LIBSTDCXX_THROW(throwBadCast,
                       "_ZSt16__throw_bad_castv",
                       "bad_cast")
KVASIR_LIBSTDCXX_THROW(throwBadAlloc,
                       "_ZSt17__throw_bad_allocv",
                       "bad_alloc")
KVASIR_LIBSTDCXX_THROW(throwBadTypeid,
                       "_ZSt18__throw_bad_typeidv",
                       "bad_typeid")
KVASIR_LIBSTDCXX_THROW(throwBadException,
                       "_ZSt21__throw_bad_exceptionv",
                       "bad_exception")
KVASIR_LIBSTDCXX_THROW(throwBadArrayNewLength,
                       "_ZSt28__throw_bad_array_new_lengthv",
                       "bad_array_new_length")
KVASIR_LIBSTDCXX_THROW_WHAT(throwLogicError,
                            "_ZSt19__throw_logic_errorPKc",
                            "logic_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwRangeError,
                            "_ZSt19__throw_range_errorPKc",
                            "range_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwDomainError,
                            "_ZSt20__throw_domain_errorPKc",
                            "domain_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwLengthError,
                            "_ZSt20__throw_length_errorPKc",
                            "length_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwOutOfRange,
                            "_ZSt20__throw_out_of_rangePKc",
                            "out_of_range")
KVASIR_LIBSTDCXX_THROW_WHAT(throwRuntimeError,
                            "_ZSt21__throw_runtime_errorPKc",
                            "runtime_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwOverflowError,
                            "_ZSt22__throw_overflow_errorPKc",
                            "overflow_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwUnderflowError,
                            "_ZSt23__throw_underflow_errorPKc",
                            "underflow_error")
KVASIR_LIBSTDCXX_THROW_WHAT(throwInvalidArgument,
                            "_ZSt24__throw_invalid_argumentPKc",
                            "invalid_argument")
    #undef KVASIR_LIBSTDCXX_THROW
    #undef KVASIR_LIBSTDCXX_THROW_WHAT

// the one with a printf format: the format itself is logged, its arguments are not expanded
[[gnu::noreturn,
  gnu::used]] void
throwOutOfRangeFmt(char const* format,
                   ...) __asm__("_ZSt24__throw_out_of_range_fmtPKcz");

[[gnu::noreturn,
  gnu::used]] void
throwOutOfRangeFmt(char const* format,
                   ...) {
    libstdcxxThrow("out_of_range", format, __builtin_return_address(0));
}
}   // namespace Kvasir::Startup::Detail
#endif
