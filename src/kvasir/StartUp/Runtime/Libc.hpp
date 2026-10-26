#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end:
// newlib's and llvm-libc's stand-ins (assert, abort, exit, atexit, syscalls, _sbrk, the OSUtil hooks) and
// libstdc++'s __glibcxx_assert_fail.
// Strong definitions that must exist once per image.
// No includes of its own on purpose: it relies on what StartUp.hpp includes before it (Runtime/Panic.hpp, uc_log,
// LinkerSymbols.hpp, Common/Interrupt.hpp, <cassert> <cerrno> <cstdlib> <cstring> <new> <string_view>), and a second
// <cassert> would re-enter libc's unguarded assert.h (re-declares uc_log::log_assert, -Wredundant-decls).
#ifdef __arm__

    // newlib's assert() pulls stdio/_write/_sbrk and libstdc++'s __throw_* pull abort -> malloc;
    // strong definitions here keep those archive members out. gnu::noreturn, not [[noreturn]]:
    // the standard attribute must be on newlib's first declaration.
    #ifdef __NEWLIB__
extern "C" {
[[gnu::noreturn,
  gnu::used]] void
__assert_func(char const* file,
              int         line,
              char const* /*func*/,
              char const* expr) {
    uc_log::log_assert(line, file, expr);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::assertion, KVASIR_DETAIL_CALLER_PC());
}

[[gnu::noreturn,
  gnu::used]] void
abort() {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "abort() called (libstdc++ __throw_* or libc)");
}

void _exit(int);   // newlib declares it in <unistd.h>, which is not included here

// newlib's own exit() runs atexit procs (none: atexit is a stub below) and then _exit(): a strong exit/_Exit here
// keeps newlib's exit.o out and gives the record the caller's site and the status - the same end as llvm-libc's.
[[gnu::noreturn,
  gnu::used]] void
exit(int status) {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "exit({}) called",
                           status);
}

[[gnu::noreturn,
  gnu::used]] void
_Exit(int status) {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "_Exit({}) called",
                           status);
}

[[gnu::noreturn,
  gnu::used]] void
_exit(int status) {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "_exit({}) called",
                           status);
}
}
    #endif
    #ifdef __NEWLIB__
// gcc has no [[clang::no_destroy]]; accept and ignore the atexit() registration of static
// destructors - firmware never exits. newlib's own atexit() would also link register_fini ->
// __libc_fini_array -> _fini, which lives in crti.o and is not linked (clang with newlib).
extern "C" {
[[gnu::used]] int atexit(void (*)()) { return 0; }
}
    #elifdef LIBC_NAMESPACE
// Whatever the compiler (gcc with llvm-libc too: it used to take the newlib branch above and link without abort/exit).
// third-party static destructors; noexcept as llvm-libc declares it
extern "C" {
[[gnu::used]] int atexit(void (*)()) noexcept { return 0; }

// llvm-libc's baremetal build has no abort(). The same end as newlib's.
[[gnu::noreturn,
  gnu::used]] void
abort() noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "abort() called");
}

// Nor exit() / _Exit().
// A firmware has nowhere to return to: the same end as abort(), with the status in the line.
[[gnu::noreturn,
  gnu::used]] void
exit(int status) noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "exit({}) called",
                           status);
}

[[gnu::noreturn,
  gnu::used]] void
_Exit(int status) noexcept {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "_Exit({}) called",
                           status);
}
}
    #endif
    #if defined(KVASIR_HEAP) && defined(__NEWLIB__)
// The full newlib (gcc's speed variants) links its stdio as soon as malloc is used: malloc needs the reent structure,
// which there holds the three standard streams. Nothing opens or prints through them, but their read/write/seek/close
// need a system call each, and libnosys' stand-ins are linker warnings ("_write is not implemented and will always
// fail"). These fail the same way, silently.
extern "C" {
int _close(int);
int _lseek(int,
           int,
           int);
int _read(int,
          char*,
          int);
int _write(int,
           char const*,
           int);

[[gnu::used]] int _close(int) { return -1; }

[[gnu::used]] int _lseek(int,
                         int,
                         int) {
    return -1;
}

[[gnu::used]] int _read(int,
                        char*,
                        int) {
    return -1;
}

[[gnu::used]] int _write(int,
                         char const*,
                         int) {
    return -1;
}
}

// A gcc build with HEAP_SIZE: newlib's malloc takes its memory through _sbrk - out of the linker's .heap, never past
// its end (without this the link fails on "undefined reference to `_sbrk'").
extern "C" {
void* _sbrk(std::ptrdiff_t increment);

[[gnu::used]] void* _sbrk(std::ptrdiff_t increment) {
    static std::uintptr_t brk{};
    auto const            start = reinterpret_cast<std::uintptr_t>(&_LINKER_heap_start_);
    auto const            end   = reinterpret_cast<std::uintptr_t>(&_LINKER_heap_end_);
    if(brk == 0) { brk = start; }
    auto const next
      = brk
      + static_cast<std::uintptr_t>(increment);   // modulo 2^32: a negative increment gives back
    if(next < start || next > end) {
        errno = ENOMEM;
        return reinterpret_cast<void*>(std::uintptr_t{0} - 1U);   // (void*)-1, newlib's "no memory"
    }
    auto const previous = brk;
    brk                 = next;
    return reinterpret_cast<void*>(previous);
}
}
    #endif
    #if defined(KVASIR_HEAP) && defined(LIBC_NAMESPACE)
// llvm-libc's heap aligns every block to max(4, alignof(max_align_t)) (Block::MIN_ALIGN), and
// operator new(size_t) promises __STDCPP_DEFAULT_NEW_ALIGNMENT__. A max_align_t weaker than that
// (lib/libc/include/stddef.h once had `typedef int`) hands 8-aligned types 4-aligned
// memory: a sanitizer type_mismatch at every such new.
static_assert(alignof(std::max_align_t) >= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
              "max_align_t is weaker than operator new's default alignment: the heap would return "
              "misaligned blocks (check the libc's stddef.h)");
// llvm-libc baremetal OSUtil hooks: heap corruption is reported via stderr and exit
extern "C" {
struct __llvm_libc_stdio_cookie {
    char unused;
};

extern __llvm_libc_stdio_cookie        __llvm_libc_stdin_cookie;
extern __llvm_libc_stdio_cookie        __llvm_libc_stdout_cookie;
extern __llvm_libc_stdio_cookie        __llvm_libc_stderr_cookie;
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie{};
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie{};
[[gnu::used]] __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie{};

long                   __llvm_libc_stdio_read(void*,
                                              char*,
                                              std::size_t);
long                   __llvm_libc_stdio_write(void*,
                                               char const*,
                                               std::size_t);
[[gnu::noreturn]] void __llvm_libc_exit(int);

[[gnu::used]] long __llvm_libc_stdio_read(void*,
                                          char*,
                                          std::size_t) {
    return 0;
}

[[gnu::used]] long __llvm_libc_stdio_write(void*,
                                           char const* buf,
                                           std::size_t size) {
    UC_LOG_SCOPE_MODULE("libc");
    // libc's own output (LIBC_ASSERT's text before its exit): not a fatal path itself
    UC_LOG_E("libc: {}", std::string_view{buf, size});
    return static_cast<long>(size);
}

[[gnu::noreturn,
  gnu::used]] void
__llvm_libc_exit(int status) {
    // llvm-libc's internal::exit is inline here (Kvasir's patch of OSUtil/exit.h): the site is the LIBC_ASSERT (or
    // other caller) itself, whose file:line is also in the "libc: ..." lines before this one
    UC_LOG_SCOPE_MODULE("libc");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "libc exit({})",
                           status);
}
}
    #endif
    #ifdef __GLIBCXX__
// libstdc++'s _GLIBCXX_ASSERTIONS reporter (the sanitize variant turns those on)
namespace std {
[[gnu::noreturn,
  gnu::used]] void
__glibcxx_assert_fail(char const* file,
                      int         line,
                      char const* /*func*/,
                      char const* cond) noexcept {
    uc_log::log_assert(line, file, cond);
    Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::assertion, KVASIR_DETAIL_CALLER_PC());
}
}   // namespace std
    #endif

#endif
