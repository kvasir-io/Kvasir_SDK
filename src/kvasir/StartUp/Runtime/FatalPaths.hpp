#pragma once
// Part of StartUp.hpp (the TU with main), included only through StartUp/Runtime/All.hpp at StartUp.hpp's end:
// __stack_chk_fail, the unhandled-interrupt end and __aeabi_idiv0 / __aeabi_ldiv0.
// Strong definitions that must exist once per image.
// No includes of its own on purpose: it relies on what StartUp.hpp includes before it (Runtime/Panic.hpp, uc_log,
// LinkerSymbols.hpp, Common/Interrupt.hpp, <cassert> <cerrno> <cstdlib> <cstring> <new> <string_view>), and a second
// <cassert> would re-enter libc's unguarded assert.h (re-declares uc_log::log_assert, -Wredundant-decls).
#ifdef __arm__

extern "C" {
[[noreturn,
  gnu::used]] inline void
__stack_chk_fail() {
    UC_LOG_SCOPE_MODULE("assert");
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::stackSmash,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "stack smashed: a -fstack-protector canary was overwritten");
}
}

[[noreturn]] inline void Kvasir::Nvic::DefaultIsrs::onIsr() {
    std::uint32_t const ipsr = Kvasir::Core::ipsr();
    auto const          irq  = static_cast<std::int32_t>(ipsr) - 16;
    // no program site caused it (pc 0): the record names the interrupt instead - the exception
    // number in detail, IRQ = detail - 16, a negative IRQ a system exception (IPSR, Armv6-M ARM
    // DDI0419E B1.4.2 / Armv8-M ARM DDI0553B.y D1.2.137)
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::unhandledInterrupt,
                           0,
                           ipsr,
                           "unhandled interrupt fired, IRQ={}",
                           irq);
}

#endif

extern "C" {
[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int);

[[noreturn,
  gnu::used]] inline int
__aeabi_idiv0(int) {
#ifdef __arm__
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::divideByZero,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "integer division by zero");
#else
    assert(false);
#endif
}

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long);

[[noreturn,
  gnu::used]] inline long long
__aeabi_ldiv0(long long) {
#ifdef __arm__
    KVASIR_DETAIL_FATAL_AT(Kvasir::Panic::Cause::divideByZero,
                           KVASIR_DETAIL_CALLER_PC(),
                           0,
                           "64-bit integer division by zero");
#else
    assert(false);
#endif
}
}
