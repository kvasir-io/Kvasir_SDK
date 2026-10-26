#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end:
// the assert reporter uc_log::log_assert, fmt's fmt::assert_fail (in log images) and Kvasir::Panic::raise / raiseAt.
// Strong definitions that must exist once per image.
// No includes of its own on purpose: it relies on what StartUp.hpp includes before it (Runtime/Panic.hpp, uc_log,
// LinkerSymbols.hpp, Common/Interrupt.hpp, <cassert> <cerrno> <cstdlib> <cstring> <new> <string_view>), and a second
// <cassert> would re-enter libc's unguarded assert.h (re-declares uc_log::log_assert, -Wredundant-decls).
#ifdef __arm__

namespace uc_log {
    // llvm-libc's and libc++'s patched headers already declare log_assert (-Wredundant-decls);
    // without any prototype clang's -Wmissing-prototypes objects instead
    #if !defined(LIBC_NAMESPACE) && !defined(_LIBCPP_VERSION)
void log_assert(int         line,
                char const* filename,
                char const* expr);
    #endif

void log_assert([[maybe_unused]] int         line,
                [[maybe_unused]] char const* filename,
                [[maybe_unused]] char const* expr) {
    UC_LOG_SCOPE_MODULE("assert");
    ::Kvasir::Panic::beginFatal();   // fatal end: its caller, the assert macro, aborts
    UC_LOG_C("libc/libc++ assert({}) {}:{}",
             std::string_view{expr},
             std::string_view{filename},
             line);
}

}   // namespace uc_log

    #ifdef FMT_VERSION
// fmt's assert reporter, which only fmt's library object defines (format-inl.h) and no firmware links: remote_fmt's
// FMT_ASSERT (and fmt's FMT_THROW without exceptions) calls it with the failed condition. The end of assert(): its
// line, then the assertion panic at the fmt function's site. Only where the log brought fmt into this TU.
[[gnu::noinline]] void fmt::assert_fail(char const* file,
                                        int         line,
                                        char const* message) {
    uc_log::log_assert(line, file, message);
    ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::assertion, KVASIR_DETAIL_CALLER_PC());
}
    #endif

namespace Kvasir::Panic {
// The one definition (this header is the TU with main's): noinline so the return address is the
// caller's, the handler looked up in dispatch<>'s instantiation, after the application's.
[[noreturn,
  gnu::noinline,
  gnu::used]] void
raise(Cause cause) {
    Detail::dispatch(Info{cause, KVASIR_DETAIL_CALLER_PC()});
}

// A library function's panic on its caller's behalf: the site the caller passes (Panic.hpp).
[[noreturn,
  gnu::noinline,
  gnu::used]] void
raiseAt(Cause         cause,
        std::uint32_t pc,
        std::uint32_t detail) {
    Detail::dispatch(Info{cause, pc, detail});
}
}   // namespace Kvasir::Panic

#endif
