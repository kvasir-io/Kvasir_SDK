#pragma once
// One end for every fatal path: assert, abort/exit, a smashed stack, a division by zero, new
// without a heap, an unhandled interrupt, a fatal sanitizer report, and (opt-in) a fault. Each path
// logs as it always did, then calls Kvasir::Panic::raise(cause); what happens next is the
// application's handler, injected like a critical-section policy (Atomic/CriticalSection.hpp):
//
//     struct AppPanic {
//         [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
//             Startup::run<Kvasir::Hook::SafeState>();   // outputs off
//             Kvasir::Panic::record(info);               // the next boot's log names it
//             Kvasir::Panic::halt(info.cause);           // or reset
//         }
//     };
//     template<>
//     inline constexpr auto Kvasir::Panic::injectedHandler<> = AppPanic{};
//
// The default records the panic in .noInit and halts on a breakpoint (bkpt 5; 6 for sanitizer
// and fault; 7 for an unhandled interrupt). The halt is inside the handler, not at the failing
// site: the site is Info::pc / lastPanic.pc (`kvasir_bench.py crash` reads it).
//
// The handler runs with interrupts masked, maybe on the fault stack, maybe with the flash busy:
// register writes only, no waiting, no logging. A panic inside the handler halts at once.

#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace Kvasir::Panic {
// The libc's assert.h and libc++'s __verbose_abort declare this enum opaquely (they cannot include
// this header) and raise static_cast<Cause>(0): assertion must stay 0 and the underlying type
// unsigned char.
enum class Cause : unsigned char {
    assertion          = 0,   // assert(), library assertions
    abort              = 1,   // abort(), exit()
    stackSmash         = 2,   // __stack_chk_fail
    divideByZero       = 3,   // __aeabi_idiv0 / __aeabi_ldiv0
    allocation         = 4,   // operator new without a heap
    unhandledInterrupt = 5,   // a vector nobody installed
    undefinedBehaviour = 6,   // a fatal sanitizer handler
    fault              = 7,   // HardFault and the other fault vectors (Panic::FaultAction)
    user               = 8,   // KVASIR_PANIC
};

[[nodiscard]] constexpr char const* name(Cause c) {
    switch(c) {
    case Cause::assertion:          return "assertion";
    case Cause::abort:              return "abort";
    case Cause::stackSmash:         return "stack smash";
    case Cause::divideByZero:       return "division by zero";
    case Cause::allocation:         return "allocation without a heap";
    case Cause::unhandledInterrupt: return "unhandled interrupt";
    case Cause::undefinedBehaviour: return "undefined behaviour";
    case Cause::fault:              return "fault";
    case Cause::user:               return "KVASIR_PANIC";
    }
    return "unknown";
}

struct Info {
    Cause         cause;
    std::uint32_t pc;   // the site: raise()'s return address, or for a library path (exit, abort,
                        // __stack_chk_fail, operator new, a sanitizer handler) its caller's, given
                        // to raiseAt(). Symbolise pc - 2, the call (a call that does not return can
                        // end its function, so pc may be in the next one)
    std::uint32_t detail{};   // what the cause has to add: the IRQ number of an unhandled
                              // interrupt, exit()'s status; 0 otherwise
};

// The last panic, kept across the reset like Fault::lastFault: the first one's cause and pc,
// and how many came since the last report.
struct Record {
    static constexpr std::uint32_t Magic = 0x9A41'C0DEU;

    std::uint32_t magic;
    std::uint32_t count;
    std::uint32_t cause;
    std::uint32_t pc;
    std::uint32_t detail;   // Info::detail
};

// volatile: uninitialised, LTO would otherwise narrow it (as Fault::lastFault)
[[gnu::section(".noInit"), gnu::used]] inline Record volatile lastPanic;

inline void record(Info const& info) {
    if(lastPanic.magic == Record::Magic) {
        lastPanic.count = lastPanic.count + 1;
        return;
    }
    lastPanic.count  = 1;
    lastPanic.cause  = static_cast<std::uint32_t>(info.cause);
    lastPanic.pc     = info.pc;
    lastPanic.detail = info.detail;
    lastPanic.magic  = Record::Magic;
}

// The record, once: cleared by reading.
inline std::optional<Record> takeLastPanic() {
    if(lastPanic.magic != Record::Magic) { return std::nullopt; }
    Record const r{.magic  = Record::Magic,
                   .count  = lastPanic.count,
                   .cause  = lastPanic.cause,
                   .pc     = lastPanic.pc,
                   .detail = lastPanic.detail};
    lastPanic.magic = 0;
    return r;
}

// The end of the line: the breakpoint number each path used before this header existed.
[[noreturn,
  gnu::always_inline]] inline void
halt([[maybe_unused]] Cause cause) {
#ifdef __arm__
    switch(cause) {
    case Cause::unhandledInterrupt:
        while(true) { asm volatile("bkpt 7" : : :); }
    case Cause::undefinedBehaviour:
    case Cause::fault:
        while(true) { asm volatile("bkpt 6" : : :); }
    default:
        while(true) { asm volatile("bkpt 5" : : :); }
    }
#else
    std::abort();
#endif
}

struct DefaultHandler {
    [[noreturn]] static void operator()(Info const& info) {
        record(info);
        halt(info.cause);
    }
};

// The application's handler: specialise with an empty argument list, from any header, before or
// after this one (the dummy pack makes the lookup happen at instantiation, in raise()).
template<typename...>
inline constexpr auto injectedHandler = DefaultHandler{};

template<typename H>
concept Handler = std::invocable<H const&, Info const&>;

namespace Detail {
    inline bool volatile entered = false;

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[noreturn]] inline void dispatch(Info const& info) {
#ifdef __arm__
        asm volatile("cpsid i" : : : "memory");   // nothing may keep driving the hardware
#endif
        if(entered) { halt(info.cause); }   // a panic inside the handler: stop here
        entered                     = true;
        Handler auto const& handler = injectedHandler<Dummy...>;
        handler(info);
        __builtin_trap();   // a handler that returns is a bug
    }
}   // namespace Detail

// Defined once, in StartUp.hpp (the header of the TU with main). The SDK's patched assert.h and
// __verbose_abort declare it already and say so with KVASIR_PANIC_RAISE_DECLARED (a second
// declaration is -Wredundant-decls); without them this is the declaration.
#ifndef KVASIR_PANIC_RAISE_DECLARED
    #define KVASIR_PANIC_RAISE_DECLARED 1
[[noreturn]] void raise(Cause cause);
#endif
// For a library function that panics on its caller's behalf (exit, __stack_chk_fail, operator new,
// a sanitizer handler): the site is the caller, pc = __builtin_return_address(0) of that function,
// not the library function itself; detail as in Info. Defined next to raise() in StartUp.hpp.
[[noreturn]] void raiseAt(Cause         cause,
                          std::uint32_t pc,
                          std::uint32_t detail = 0);

// For Fault::Handler's TFaultAction: a fault goes the same way as every other fatal path.
struct FaultAction {
    [[noreturn]] void operator()() { raise(Cause::fault); }
};
}   // namespace Kvasir::Panic

// Log the reason, then panic. The message goes through the catalog like any log line.
#define KVASIR_PANIC(...)                                     \
    do {                                                      \
        UC_LOG_C(__VA_ARGS__);                                \
        ::Kvasir::Panic::raise(::Kvasir::Panic::Cause::user); \
    } while(false)
