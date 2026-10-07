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

#include "kvasir/StartUp/LinkerSymbols.hpp"
#include "kvasir/Util/CrashRecord.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <type_traits>

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
    checkFailed        = 9,   // KVASIR_CHECK*, a KVASIR_SOFT_CHECK that escalates (Util/Check.hpp)
    timeout = 10,   // a bounded register wait under OnTimeout::Panic; detail = the register (Register/Wait.hpp)
    bootLoop = 11,   // the boot guard's RaisePanic policy; detail = the failed runs in a row (BootGuard.hpp)
    healthCheck = 12,   // Health::Supervisor's RaisePanic policy; detail = the starved check's index (Health.hpp)
    imageCorrupt = 13,   // ImageCheck's Panic policies: the image (in flash, or a RAM image's code) differs from its
                         // build; detail = the CRC computed (ImageCheck.hpp)
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
    case Cause::checkFailed:        return "check failed";
    case Cause::timeout:            return "register wait timed out";
    case Cause::bootLoop:           return "boot loop";
    case Cause::healthCheck:        return "health check starved";
    case Cause::imageCorrupt:       return "image CRC mismatch";
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

// The record under CrashRecord::Full: CRC-checked (Persistent.hpp), with the core and the stack
// pointer in raise(). Referenced only by the Full path, so a Legacy image does not have it.
struct RecordTag {
    static constexpr std::uint32_t id      = persistentId("KPNC");
    static constexpr std::uint16_t version = 2;
};

struct FullRecord {
    std::uint32_t count;
    std::uint32_t cause;
    std::uint32_t pc;
    std::uint32_t detail;
    std::uint32_t core;   // 0, or 1 when raised on the secondary core's stack
    std::uint32_t sp;     // the stack pointer where the record was written
};

[[KVASIR_SECTION(".noInit")]] inline Persistent<FullRecord, RecordTag> lastPanicV2;

namespace Detail {
    template<typename... Dummy>
    inline constexpr bool fullPolicy = CrashRecord::Detail::isFull<
      std::remove_cvref_t<decltype(CrashRecord::injectedPolicy<Dummy...>)>>;

#ifdef __arm__
    [[gnu::always_inline]] inline std::uint32_t stackPointer() {
        std::uint32_t sp;
        asm volatile("mov %0, sp" : "=r"(sp));
        return sp;
    }

    // Which core runs this: the one whose stack holds sp. The SDK knows no chip's core id register,
    // and on a single-core chip the secondary stack is empty.
    inline std::uint32_t coreOf(std::uint32_t sp) {
        return sp >= reinterpret_cast<std::uintptr_t>(_LINKER_stack1_start_)
                && sp < reinterpret_cast<std::uintptr_t>(_LINKER_stack1_end_)
               ? 1U
               : 0U;
    }
#else
    inline std::uint32_t stackPointer() { return 0; }

    inline std::uint32_t coreOf(std::uint32_t) { return 0; }
#endif

    /// The cause of the panic record, without taking it (the boot guard).
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    std::optional<std::uint32_t> recordedCause() {
        if constexpr(fullPolicy<Dummy...>) {
            auto const r = lastPanicV2.load();
            return r ? std::optional<std::uint32_t>{r->cause} : std::nullopt;
        } else {
            return lastPanic.magic == Record::Magic ? std::optional<std::uint32_t>{lastPanic.cause}
                                                    : std::nullopt;
        }
    }

    /// Whether a panic record is there, without taking it (the fault handler, the boot guard).
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    bool panicRecorded() {
        if constexpr(fullPolicy<Dummy...>) {
            return lastPanicV2.valid();
        } else {
            return lastPanic.magic == Record::Magic;
        }
    }
}   // namespace Detail

// The first panic since the last report is kept, later ones only counted. A template so the policy
// is looked up where it is used, after the application's specialisation.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline void record(Info const& info) {
    if constexpr(Detail::fullPolicy<Dummy...>) {
        if(auto old = lastPanicV2.load()) {
            old->count = old->count + 1;
            lastPanicV2.store(*old);
            return;
        }
        auto const sp = Detail::stackPointer();
        lastPanicV2.store(FullRecord{.count  = 1,
                                     .cause  = static_cast<std::uint32_t>(info.cause),
                                     .pc     = info.pc,
                                     .detail = info.detail,
                                     .core   = Detail::coreOf(sp),
                                     .sp     = sp});
    } else {
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
}

// The record, once: cleared by reading. The same shape under either policy.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline std::optional<Record> takeLastPanic() {
    if constexpr(Detail::fullPolicy<Dummy...>) {
        auto const r = lastPanicV2.take();
        if(!r) { return std::nullopt; }
        return Record{.magic  = Record::Magic,
                      .count  = r->count,
                      .cause  = r->cause,
                      .pc     = r->pc,
                      .detail = r->detail};
    } else {
        if(lastPanic.magic != Record::Magic) { return std::nullopt; }
        Record const r{.magic  = Record::Magic,
                       .count  = lastPanic.count,
                       .cause  = lastPanic.cause,
                       .pc     = lastPanic.pc,
                       .detail = lastPanic.detail};
        lastPanic.magic = 0;
        return r;
    }
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
    // a template, so record()'s policy lookup waits for the call (dispatch, at instantiation)
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[noreturn]] static void operator()(Info const& info) {
        record<Dummy...>(info);
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
