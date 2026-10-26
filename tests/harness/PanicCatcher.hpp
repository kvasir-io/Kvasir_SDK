// Catching a Kvasir panic in a host test: the host stand-ins for Kvasir::Panic::raise/raiseAt (the target's come
// from StartUp/Runtime/PanicEntry.hpp, which a host does not compile) and a handler that leaves the panic by longjmp.
//
// Define-once: the stand-ins are not inline, so include this header from the ONE translation unit of the test
// program. The handler is installed by the test itself, at the place it wants it (a late specialisation still wins):
//
//     template<>
//     struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
//         using type = Kvasir::Test::CatchPanic;
//     };
//
// Under TSan a case that catches a panic is left out: `#ifndef KVASIR_TEST_NO_LONGJMP` around it (harness/NoLongjmp.hpp).
#pragma once

#include "NoLongjmp.hpp"   // its own directory: tests/ is no include path
#include "kvasir/Runtime/Panic.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>

namespace Kvasir::Panic {
[[noreturn,
  gnu::noinline]] void
raise(Cause cause) {
    Detail::dispatch(Info{
      cause,
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)))});
}

[[noreturn]] void raiseAt(Cause         cause,
                          std::uint32_t pc,
                          std::uint32_t detail) {
    Detail::dispatch(Info{cause, pc, detail});
}
}   // namespace Kvasir::Panic

namespace Kvasir::Test {
inline std::jmp_buf                       panicBack;
inline std::optional<Kvasir::Panic::Info> panicSeen;

// the handler: keeps the panic's Info and jumps back to the setjmp in panicOf (or the test's own)
struct CatchPanic {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        panicSeen = info;
        std::longjmp(panicBack, 1);
    }
};

// Runs f; the panic it raised (the whole Info: cause, pc, detail), or nullopt. Re-arms Panic::Detail::entered.
template<typename F>
std::optional<Kvasir::Panic::Info> panicOf(F&& f) {
    panicSeen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(panicBack) == 0) { f(); }
    return panicSeen;
}
}   // namespace Kvasir::Test
