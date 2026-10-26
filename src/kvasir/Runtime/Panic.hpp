#pragma once
// One end for every fatal path: assert, abort/exit, a smashed stack, a division by zero, new
// without a heap, an unhandled interrupt, a fatal sanitizer report, a fault (Fault::HandlerFor's
// default reaction, Fault::OnFault::Panic). Each path starts with beginFatal(), logs its line, then
// calls Kvasir::Panic::raise(cause); what happens next is the image's Panic::PanicReaction, a late hook
// (the table in kvasir/Port/Hooks.hpp) the application may specialise from any header every TU includes:
//
//     template<>
//     struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
//         using type = Kvasir::Panic::OnPanic::Halt;   // or OnPanic::Wait, OnPanic::Reset<R>, its own
//     };
//
// The default (OnPanic::Default) records the panic in .noInit and reboots the chip through its
// Port::Reboot, announced to a log printer - with or without a debugger attached: an image
// recovers by itself, and the next boot's report names the panic. On a chip package without a
// reboot it halts. The stock reactions, every one records first:
//   - OnPanic::Reset<Reboot>: record, uc_log::drain(), a bounded wait for a printer that heard the
//     previous fatal reset and is not back yet (3 s), then Reboot{}();
//   - OnPanic::Wait: record, then spin until a watchdog resets the chip;
//   - OnPanic::Halt: record, then the path's breakpoint (bkpt 5; 6 for sanitizer and fault; 7 for
//     an unhandled interrupt) - for a debug session; without halting debug that is a lockup.
// The site is Info::pc / the record's pc (`kvasir_bench.py crash` reads it), not where it stops.
//
// The handler runs with interrupts masked, maybe on the fault stack, maybe with the flash busy:
// register writes only, no logging; the waits are OnPanic::Reset's two, both bounded. A panic
// inside the handler or the fatal start takes the end without the handler (Detail::stop): the
// reboot, or the halt under OnPanic::Halt.
//
// Every fatal path starts with beginFatal(), before its log line: with uc_log's
// LossPolicy::Counted, the log lines from there on wait for room while a host reads (bounded),
// so the panic's own line is not dropped by a full ring. Nothing otherwise. A fatal site writes
// that order with KVASIR_DETAIL_FATAL_AT(cause, pc, detail, fmt, args...) or
// KVASIR_DETAIL_FATAL(log, end), which keep it (tools/check_fatal_sites.py checks every site).
//
// The image's fatal start, Panic::FatalStartReaction (a late hook as PanicReaction), runs first in beginFatal()
// - before that wait - and again in the dispatch after interrupts are masked, before the handler
// (so a raise that never ran beginFatal runs it too). A product's is its safe state, one line
// after `using Startup = ...` (kvasir/StartUp/SafeState.hpp; Start<S> refuses a list with
// Hook::SafeState entries and no such line):
//
//     template<>
//     struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
//         using type = Kvasir::Startup::SafeStateOf<::Startup>;
//     };
//
// It is Hook::SafeState's contract: register writes (a bounded busy wait on the part's own
// registers at most), any context, before the part's init too, idempotent.
//
// Which condition is fatal at all, and which log level goes with which: the error classes in
// Runtime/Check.hpp's header (crit only on a fatal path, after beginFatal()).

#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/LinkerSymbols.hpp"
#include "kvasir/Runtime/AnnouncedReset.hpp"
#include "kvasir/Runtime/CrashRecord.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"
#ifdef __arm__
    #include "core/InterruptMask.hpp"
#endif

#if defined(USE_UC_LOG) && __has_include("uc_log/Runtime.hpp")
    #include "uc_log/Runtime.hpp"
    #define KVASIR_PANIC_DETAIL_UC_LOG 1
#endif

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
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
    fault              = 7,   // HardFault and the other fault vectors (Fault::OnFault::Panic)
    user               = 8,   // KVASIR_PANIC
    checkFailed = 9,   // KVASIR_CHECK*, a KVASIR_SOFT_CHECK that escalates (Runtime/Check.hpp)
    timeout = 10,   // a bounded register wait under OnTimeout::Panic; detail = the register (Register/Wait.hpp)
    bootLoop = 11,   // the boot guard's RaisePanic policy; detail = the failed runs in a row (BootGuard.hpp)
    healthCheck = 12,   // Health::Supervisor's RaisePanic policy; detail = the starved check's index (Health.hpp)
    imageCorrupt = 13,   // ImageCheck's Panic policies: the image (in flash, or a RAM image's code) differs from its
                         // build; detail = the CRC computed (ImageCheck.hpp)
    clockFailure = 14,   // Clocks::Resus with panicOnFailure: clk_sys stopped and the restart did not verify;
                         // detail = clk_sys in kHz after it (chip_rp2350 ClockOutput.hpp)
    secondaryCore = 15,   // a SecondaryCore in the Startup list did not come up: handshake, rendezvous or ready
                          // wait failed (StartUp/SecondaryCore.hpp)
    stackOverflow = 16,   // StackProtector's sentinel at the stack bottom overwritten; pc 0 (found a turn later, no
    // program site), detail = the sentinel words overwritten (Runtime/StackProtector.hpp)
    clockInit = 17,   // the chip's clock init at boot did not finish (a crystal that did not start, a PLL that did
    // not lock - each wait bounded); detail = the chip's init result (RP: Clocks::InitResult,
    // chip_rp_common clock_config.hpp)
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
    case Cause::clockFailure:       return "clock restart failed";
    case Cause::secondaryCore:      return "secondary core did not come up";
    case Cause::stackOverflow:      return "stack overflow";
    case Cause::clockInit:          return "clock init failed";
    }
    return "unknown";
}

/// One past the last Cause: the first value name() does not know. The host's copy of the table (uc_log's
/// printer/TargetRecords.hpp panicCauses) is checked against it by tests/records_cross_tests.cpp, and so
/// is an application's API enum where it mirrors Cause.
inline constexpr std::size_t causeCount = [] {
    std::size_t n = 0;
    while(std::string_view{name(static_cast<Cause>(n))} != "unknown") { ++n; }
    return n;
}();

struct Info {
    Cause         cause;
    std::uint32_t pc;   // the site: raise()'s return address, or for a library path (exit, abort,
                        // __stack_chk_fail, operator new, a sanitizer handler) its caller's, given
                        // to raiseAt(). Symbolise pc - 2, the call (a call that does not return can
                        // end its function, so pc may be in the next one)
    std::uint32_t detail{};   // what the cause has to add: the IRQ number of an unhandled
                              // interrupt, exit()'s status; 0 otherwise
};

// The panic record (CrashRecord.hpp): CRC-checked (Persistent.hpp), with the core and the stack
// pointer in raise(), kept across the reset like Fault::lastFault. The first panic's cause, site and time, and how
// many came since the last report - what takeLastPanic() gives and logPanic() prints.
struct RecordTag {
    static constexpr std::uint32_t id = persistentId("KPNC");
};

struct FullRecord {
    std::uint32_t count;
    std::uint32_t cause;
    std::uint32_t pc;
    std::uint32_t detail;
    std::uint32_t core;   // 0, or 1 when raised on the secondary core's stack
    std::uint32_t sp;     // the stack pointer where the record was written
    // The chip's free-running microsecond counter when the record was written (Port::FreeRunningUs; RP: the TIMER
    // since its release in that boot), two words so the record stays whole words; 0 on a chip without one.
    std::uint32_t timeLow;
    std::uint32_t timeHigh;

    [[nodiscard]] constexpr std::uint64_t timeUs() const {
        return (static_cast<std::uint64_t>(timeHigh) << 32U) | timeLow;
    }
};

[[KVASIR_NOINIT]] inline Persistent<FullRecord, RecordTag> lastPanic;

/// The current stack pointer (0 on the host).
#ifdef __arm__
[[gnu::always_inline]] inline std::uint32_t stackPointer() {
    std::uint32_t sp{};
    asm volatile("mov %0, sp" : "=r"(sp));
    return sp;
}
#else
inline std::uint32_t stackPointer() { return 0; }
#endif

namespace Detail {
    template<typename... Dep>
    using FreeRunningUsOf = Port::LateT<Port::FreeRunningUs<Tag::User>, Dep...>;

    /// Whether the chip has a free-running microsecond counter (Port::FreeRunningUs).
    template<typename... Dep>
    inline constexpr bool hasFreeRunningUs = requires { sizeof(FreeRunningUsOf<Dep...>); };

    /// The chip's free-running microsecond counter now, 0 without one.
    template<typename... Dep>
    [[gnu::always_inline]] inline std::uint64_t freeRunningUs() {
        if constexpr(hasFreeRunningUs<Dep...>) {
            return FreeRunningUsOf<Dep...>{}();
        } else {
            return 0;
        }
    }

#ifdef __arm__
    // Which core runs this: the one whose stack holds sp. The SDK knows no chip's core id register,
    // and on a single-core chip the secondary stack is empty.
    inline std::uint32_t coreOf(std::uint32_t sp) {
        return sp >= reinterpret_cast<std::uintptr_t>(_LINKER_stack1_start_)
                && sp < reinterpret_cast<std::uintptr_t>(_LINKER_stack1_end_)
               ? 1U
               : 0U;
    }
#else
    inline std::uint32_t coreOf(std::uint32_t) { return 0; }
#endif
}   // namespace Detail
}   // namespace Kvasir::Panic

// The format's panic part (CrashRecord.hpp): where a panic goes and how the next boot gets it back. The bodies are
// the record's own, so a format's part is all of its panic behaviour.
namespace Kvasir::CrashRecord {
template<Options O>
struct Full<O>::Panic {
    static bool recorded() { return ::Kvasir::Panic::lastPanic.valid(); }

    static std::optional<std::uint32_t> cause() {
        auto const r = ::Kvasir::Panic::lastPanic.load();
        return r ? std::optional<std::uint32_t>{r->cause} : std::nullopt;
    }

    static void record(::Kvasir::Panic::Info const& info) {
        auto& lastPanic = ::Kvasir::Panic::lastPanic;
        if(auto old = lastPanic.load()) {
            old->count = old->count + 1;
            lastPanic.store(*old);
            return;
        }
        auto const sp   = ::Kvasir::Panic::stackPointer();
        auto const time = ::Kvasir::Panic::Detail::freeRunningUs<Full<O>>();
        lastPanic.store(
          ::Kvasir::Panic::FullRecord{.count    = 1,
                                      .cause    = static_cast<std::uint32_t>(info.cause),
                                      .pc       = info.pc,
                                      .detail   = info.detail,
                                      .core     = ::Kvasir::Panic::Detail::coreOf(sp),
                                      .sp       = sp,
                                      .timeLow  = static_cast<std::uint32_t>(time),
                                      .timeHigh = static_cast<std::uint32_t>(time >> 32U)});
    }

    static std::optional<::Kvasir::Panic::FullRecord> take() {
        return ::Kvasir::Panic::lastPanic.take();
    }
};

// What a format's Panic part provides (an injected format is checked against it where its part is used).
template<typename P>
concept PanicPart = requires(::Kvasir::Panic::Info const& info) {
    { P::recorded() } -> std::same_as<bool>;
    { P::cause() } -> std::same_as<std::optional<std::uint32_t>>;
    { P::record(info) } -> std::same_as<void>;
    { P::take() } -> std::same_as<std::optional<::Kvasir::Panic::FullRecord>>;
};
}   // namespace Kvasir::CrashRecord

namespace Kvasir::Panic {
namespace Detail {
    template<typename... Dummy>
    using PanicPartOf = typename CrashRecord::Records<Info, Dummy...>::Panic;

    template<typename... Dummy>
    consteval bool panicPartOk() {
        static_assert(
          CrashRecord::PanicPart<PanicPartOf<Dummy...>>,
          "the injected CrashRecord format's Panic part lacks recorded(), cause(), record(Info) or "
          "take() -> std::optional<Panic::FullRecord> (CrashRecord::PanicPart, Panic.hpp)");
        return true;
    }
}   // namespace Detail

// The first panic since the last report is kept, later ones only counted. A template so the format
// is looked up where it is used, after the application's specialisation.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline void record(Info const& info) {
    static_assert(Detail::panicPartOk<Dummy...>());
    Detail::PanicPartOf<Dummy...>::record(info);
}

// The record, once: cleared by reading. The same shape under every format.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline std::optional<FullRecord> takeLastPanic() {
    static_assert(Detail::panicPartOk<Dummy...>());
    return Detail::PanicPartOf<Dummy...>::take();
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

// The stock reactions (PanicReaction::type). Templates with their own empty pack, so record()'s format lookup waits for
// the call (dispatch, at instantiation).
namespace OnPanic {
    /// Record, then halt on the path's breakpoint: the debugging side, by name. Opt-in per image (the default resets).
    /// Without halting debug the breakpoint escalates to a HardFault and the core locks up until a watchdog or a power
    /// cycle.
    struct Halt {
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        [[noreturn]] static void operator()(Info const& info) {
            record<Dummy...>(info);
            halt(info.cause);
        }
    };

    /// Record, then spin until a watchdog resets the chip: for an image whose watchdog the panic stops feeding.
    struct Wait {
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        [[noreturn]] static void operator()(Info const& info) {
            record<Dummy...>(info);
            while(true) { asm volatile(""); }   // a side effect: the loop may not be assumed to end
        }
    };
}   // namespace OnPanic

namespace Detail {
    // "The last fatal reset was announced to a listening printer": .noInit, so the boot after it still knows. A
    // panic soon after such a reset finds the printer away or reconnecting; resetting into that reconnect is the
    // unannounced reset announce() exists to avoid. A magic, not a flag: the word is garbage after a power-on.
    inline constexpr std::uint32_t fatalResetHeardMagic = 0x4E52'4146U;   // "FARN" in memory

    [[KVASIR_NOINIT]] inline std::uint32_t volatile fatalResetHeard;

    /// Before a fatal reset: let a reading host take the log (uc_log::drain(), nothing without a counting backend),
    /// and wait, bounded (3 s at the fastest clock), for a printer that heard the previous fatal reset and is not
    /// back yet. Nothing without uc_log.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    inline void beforeFatalReset() {
#ifdef KVASIR_PANIC_DETAIL_UC_LOG
        uc_log::drain<std::conditional_t<sizeof...(Dummy) == 0, uc_log::Tag::User, void>>();
#endif
#ifdef USE_UC_LOG
        if(fatalResetHeard == fatalResetHeardMagic && !AnnouncedReset::printerListening()) {
            (void)AnnouncedReset::waitForPrinter(3'000'000);
        }
        fatalResetHeard = AnnouncedReset::printerListening() ? fatalResetHeardMagic : 0U;
#endif
    }
}   // namespace Detail

namespace OnPanic {
    /// Record, let a reading host take the log, then Reboot{}() - announced to the printer (the chip's Port::Reboot
    /// does that). Reboot: a type with `[[noreturn]] void operator()() const`.
    template<typename Reboot>
    struct Reset {
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        [[noreturn]] static void operator()(Info const& info) {
            record<Dummy...>(info);
            Detail::beforeFatalReset<Dummy...>();
            Reboot{}();
        }
    };
}   // namespace OnPanic

namespace Detail {
    /// The chip's reboot (Port::Reboot<Tag::User>, kvasir/Port/Hooks.hpp), named where a fatal end runs.
    template<typename... Dummy>
    using RebootOf = Port::LateT<Port::Reboot<Tag::User>, Dummy...>;

    /// Whether the chip has a reboot: a chip package without the specialisation keeps the halt.
    template<typename... Dummy>
    inline constexpr bool canReboot = requires { sizeof(RebootOf<Dummy...>); };

    enum class End : unsigned char { halt, reset };

    /// The default end, the whole decision: reset whenever the chip can, with or without a debugger (a debug
    /// session that wants the halt names OnPanic::Halt).
    constexpr End defaultEnd(bool chipCanReboot) { return chipCanReboot ? End::reset : End::halt; }
}   // namespace Detail

namespace OnPanic {
    /// The default (PanicReaction's primary): record, then the chip's reboot - an image without a reaction of its own
    /// recovers by itself. On a chip without Port::Reboot: Halt.
    struct Default {
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        [[noreturn]] static void operator()(Info const& info) {
            if constexpr(Detail::defaultEnd(Detail::canReboot<Dummy...>) == Detail::End::reset) {
                Reset<Detail::RebootOf<Dummy...>>::template operator()<Dummy...>(info);
            } else {
                Halt::template operator()<Dummy...>(info);
            }
        }
    };

    /// Record, then the application's `[[noreturn]] void fn(Info const&)` (interrupts masked, maybe on the fault
    /// stack: register writes, then its own end - a reboot, a wait for the watchdog). A fn that returns is the
    /// dispatch's trap.
    template<auto Fn>
    struct Call {
        template<typename... Dummy>
            requires(sizeof...(Dummy) == 0)
        [[noreturn]] static void operator()(Info const& info) {
            record<Dummy...>(info);
            Fn(info);
            __builtin_trap();
        }
    };
}   // namespace OnPanic

// What beginFatal() does first (header comment): nothing unless a FatalStartReaction names something. It runs wherever
// a fatal path starts (fault handler, interrupts masked, smashed stack): register writes only, no log, no lock.
struct NoFatalStart {
    static void operator()() {}
};

// The image's two panic hooks, late port hooks (kvasir/Port/Hooks.hpp): the application specialises them for
// Kvasir::Tag::User, from any header every translation unit includes, and names its choice as `type`:
//     template<>
//     struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> { using type = Kvasir::Panic::OnPanic::Halt; };
// They are read through Port::LateT where a fatal path runs, so the specialisation may come after this header.
// PanicReaction::type: the handler, `[[noreturn]] static void operator()(Info const&)` (concept Handler below).
template<typename T, typename... Ts>
struct PanicReaction {
    using type = OnPanic::Default;
};

// FatalStartReaction::type: `static void operator()()`, run first by beginFatal() and again by the dispatch (header
// comment); usually Startup::SafeStateOf<Startup> (kvasir/StartUp/SafeState.hpp).
template<typename T, typename... Ts>
struct FatalStartReaction {
    using type = NoFatalStart;
};

template<typename H>
concept Handler = std::invocable<H const&, Info const&>;

namespace Detail {
    template<typename... Dummy>
    using ReactionOf = typename Port::LateT<PanicReaction<Tag::User>, Dummy...>::type;

    template<typename... Dummy>
    using FatalStartOf = typename Port::LateT<FatalStartReaction<Tag::User>, Dummy...>::type;

    /// The image's fatal start, called: beginFatal(), dispatch(), the fault handler's clean-up.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[gnu::always_inline]] inline void runFatalStart() {
        FatalStartOf<Dummy...>{}();
    }

    inline bool volatile entered = false;

    template<typename... Dummy>
    using StopOtherCoreOf = Port::LateT<Port::StopOtherCore<Tag::User>, Dummy...>;

    /// The chip's Port::StopOtherCore, where it has one; nothing otherwise.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[gnu::always_inline]] inline void stopOtherCore() {
        if constexpr(requires { sizeof(StopOtherCoreOf<Dummy...>); }) {
            StopOtherCoreOf<Dummy...>{}();
        }
    }

    /// A panic inside the handler or the fatal start: the image's end without the handler - the chip's reboot,
    /// unless the image halts (OnPanic::Halt) or the chip has no reboot. No record, no drain: those may be what
    /// panicked.
    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[noreturn]] inline void stop(Cause cause) {
        if constexpr(!std::is_same_v<ReactionOf<Dummy...>, OnPanic::Halt>
                     && defaultEnd(canReboot<Dummy...>) == End::reset)
        {
            RebootOf<Dummy...>{}();
        } else {
            halt(cause);
        }
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    [[noreturn]] inline void dispatch(Info const& info) {
#ifdef __arm__
        Core::maskIrqs();   // nothing may keep driving the hardware
#endif
        // a panic inside the handler, or inside the fatal start: no handler again
        if(entered) { stop<Dummy...>(info.cause); }
        entered = true;   // before the start, so a panic inside it stops above and does not recurse
        // the outputs off again, now with interrupts masked (also for a raise that never ran beginFatal)
        runFatalStart<Dummy...>();
        // and no other core turns one on again (the chip's Port::StopOtherCore, if it has more than one)
        stopOtherCore<Dummy...>();
        Handler auto const handler = ReactionOf<Dummy...>{};
        handler(info);
        __builtin_trap();   // a handler that returns is a bug
    }
}   // namespace Detail

// Defined once, in StartUp/Runtime/PanicEntry.hpp (included by StartUp.hpp, the header of the TU with main). The
// SDK's patched assert.h and __verbose_abort declare it already and say so with KVASIR_PANIC_RAISE_DECLARED (a second
// declaration is -Wredundant-decls); without them this is the declaration.
#ifndef KVASIR_PANIC_RAISE_DECLARED
    #define KVASIR_PANIC_RAISE_DECLARED 1
[[noreturn]] void raise(Cause cause);
#endif
// For a library function that panics on its caller's behalf (exit, __stack_chk_fail, operator new,
// a sanitizer handler): the site is the caller, pc = __builtin_return_address(0) of that function,
// not the library function itself; detail as in Info. Defined next to raise() in
// StartUp/Runtime/PanicEntry.hpp.
[[noreturn]] void raiseAt(Cause         cause,
                          std::uint32_t pc,
                          std::uint32_t detail = 0);

// The first statement of every fatal path, before its log line (header comment). A template so the
// log backend and the fatal start are looked up where used: the application's may come later.
template<typename... Dummy>
    requires(sizeof...(Dummy) == 0)
inline void beginFatal() {
    Detail::runFatalStart<Dummy...>();
#ifdef KVASIR_PANIC_DETAIL_UC_LOG
    uc_log::panicMode<std::conditional_t<sizeof...(Dummy) == 0, uc_log::Tag::User, void>>();
#endif
}

}   // namespace Kvasir::Panic

namespace Kvasir::Panic::Detail {
// A site as Info and FullRecord store it: the low 32 bits of a code address. Compiles on a 64-bit host too, where
// reinterpret_cast<std::uint32_t>(pointer) is ill-formed.
[[gnu::always_inline]] inline std::uint32_t sitePc(void const* p) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
}

// The active exception's number (IPSR): which fault vector a fault came through. 0 on a host.
[[gnu::always_inline]] inline std::uint32_t activeException() {
#ifdef __arm__
    return Kvasir::Core::exceptionNumber();
#else
    return 0;
#endif
}
}   // namespace Kvasir::Panic::Detail

// The site of a library end: the caller of the function this expands in. A macro on purpose - in a helper the
// helper's frame would be the site.
#define KVASIR_DETAIL_CALLER_PC() ::Kvasir::Panic::Detail::sitePc(__builtin_return_address(0))

// The order of every fatal path, written once: the start (the injected fatal start, uc_log's panic mode), the line,
// the end. `log` is one statement (or empty); `end` does not return (raise, raiseAt, Check::Detail::failed()).
// No module scope here: the site keeps its own UC_LOG_SCOPE_MODULE.
// `log ## __VA_ARGS__` (always empty) keeps `log` out of the arguments' pre-expansion: it expands where it is used,
// with the rest of the body. Pre-expanded, the _Pragma lines inside a uc_log call (the catalog site's
// -Wc2y-extensions push/pop around __COUNTER__) would be dropped while __COUNTER__ itself still expanded - a
// -Wc2y-extensions error in every image with a log catalog.
#define KVASIR_DETAIL_FATAL(log, end, ...) \
    do {                                   \
        ::Kvasir::Panic::beginFatal();     \
        log##__VA_ARGS__;                  \
        end;                               \
    } while(false)

// The common case: an optional crit line through UC_LOG_C, then raiseAt.
//     KVASIR_DETAIL_FATAL_AT(Cause::abort, KVASIR_DETAIL_CALLER_PC(), status, "exit({}) called", status);
#define KVASIR_DETAIL_FATAL_AT(cause, pc, detail, ...)                     \
    KVASIR_DETAIL_FATAL(__VA_OPT__(UC_LOG_C(__VA_ARGS__)),                 \
                        ::Kvasir::Panic::raiseAt((cause), (pc), (detail)))

// Log the reason, then panic. The message goes through the catalog like any log line. raise(), not raiseAt(): the
// site is this line (raise is noinline and takes its own return address).
#define KVASIR_PANIC(...) \
    KVASIR_DETAIL_FATAL(UC_LOG_C(__VA_ARGS__), ::Kvasir::Panic::raise(::Kvasir::Panic::Cause::user))
