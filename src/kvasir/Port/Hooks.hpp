#pragma once
// Every hook - a choice a chip package or an application makes for the whole image - has one form: a class template
// `template<typename T, typename... Ts> struct X;` that the chip or the application specialises for Kvasir::Tag::User,
// from a header every translation unit of the image includes, and that the SDK names through Port::LateT below, so
// the specialisation is found where the image instantiates the user, not where the SDK header is parsed (a
// specialisation after that first instantiation is "explicit specialization after instantiation"). A choice among
// types names it as `type`; a reaction to an event is one of the event's `On<Event>::` structs or the application's
// own:
//
//     template<>
//     struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> { using type = Kvasir::Health::OnStarvation::Log; };
//
// The hooks (header; default; what the specialisation names):
//
//   the chip package's
//   Port::Reboot                     here; declared only (no reboot)     `[[noreturn]] void operator()() const`
//   Port::StopOtherCore              here; declared only (one core)      `void operator()() const`
//   Port::CrossCoreLock              here; declared only (one core, or   `static void acquire()`, `static void release()`
//                                    a core with exclusive accesses)
//   Port::ResetCause                 here; declared only (an error)      `operator()() const` -> the chip's reset cause
//                                                                        (Boot::Report; the chip's PM::resetCause())
//   Port::RestartedAfterJump         here; declared only (no line)       `bool operator()() const`
//   Port::StartNotes                 here; declared only (no line)       `void operator()() const`
//   Port::UnmappedAddress           here; declared only (no fault       `static constexpr std::uintptr_t value`
//                                    injection)                          (Runtime/FaultInject.hpp)
//   Port::FreeRunningUs              here; declared only (no time in     `std::uint64_t operator()() const`
//                                    the panic record)                   (Runtime/Panic.hpp)
//   Startup::FirstInitStep           here; declared only (an error)      the first step after the core's clean-up
//   Startup::StackGuardEntropy       here; the constant 0xdeadc0de       `std::uint32_t operator()() const`
//   Startup::ExtraMemoryInit         here; nothing                       `void operator()() const`
//   Startup::SecondaryCoreInit       here; declared only (an error)      `cpacrEnable`, `void operator()()`
//   Startup::ChipEntries             StartUp/Entry.hpp; NoChipEntries    a set with `template<typename ClockSettings>
//                                                                        using For = brigand::list<...>`: entries
//                                                                        every primary Startup list runs before its
//                                                                        own (RP: Clocks::DegradedBoot<ClockSettings>)
//   Nvic::InterruptOffsetTraits      Port/Interrupts.hpp; declared only  begin, end, disabled, ... (chip/Interrupt.hpp)
//   Register::CpuHzCeiling           here; 0 (an error)                  `static constexpr std::uint64_t value`
//   Register::ExecuteSeam            Register/Exec.hpp; the plain        `unsigned operator()(unsigned in)` for one
//                                    access (Detail::RegisterExec)       access kind on the chip's addresses (RP alias
//                                                                        stores, STM32 ODR toggle); the chip names its
//                                                                        header in KVASIR_CHIP_REGISTER_SEAM, which
//                                                                        Exec.hpp includes at its end
//   Hal::BlockGates                  Hal/Block.hpp; declared only (no    `using type = Hal::BlockTable<rows...>` (the
//                                    gate check)                         chip's chip/Blocks.hpp)
//   Startup::Diagnostics::           StartUp/InterruptNames.hpp; no      `static consteval std::string_view of(int)`
//   DeviceInterruptNames             device names                        (generated peripherals/InterruptNames.hpp)
//
//   the application's (`using type = ...;`)
//   Panic::PanicReaction             Runtime/Panic.hpp; OnPanic::Default    OnPanic::{Default, Halt, Wait, Reset<Reboot>,
//                                                                        Call<&fn>}
//   Panic::FatalStartReaction        Runtime/Panic.hpp; NoFatalStart        Startup::SafeStateOf<::Startup>, a `static
//                                                                        void operator()()` (register writes only)
//   Fault::FaultReaction             Runtime/FaultReaction.hpp;             OnFault::{Panic, Halt, Call<&fn>} (every
//                                    OnFault::Panic                      HandlerFor's end)
//   Register::WaitPolicy             Register/Wait.hpp;                  OnTimeout::{Unbounded, Report, Ignore, Log,
//                                    OnTimeout::Unbounded                Count, Call<&fn>, Panic, PanicAfter<N>}
//   Atomic::CriticalSectionPolicy    Atomic/CriticalSection.hpp;         Policy::{IrqMask, IrqMaskAndLock} (host:
//                                    Policy::Default (by core count)     MutexPerTag), a CriticalSectionPolicyShape
//   Startup::HookProfiler            Port/Extends.hpp; NoHookProfiler   Profile::HookProfiler<Cfg>
//   CrashRecord::RecordFormat        Runtime/CrashRecord.hpp; Full<>        Full<Options>, a Format of its own
//   Health::StarvationReaction       Health/Policies.hpp;           OnStarvation::{StopFeeding, Ignore, Log,
//                                    OnStarvation::StopFeeding           Count, Call<&fn, keepFeeding>, Panic,
//                                                                        PanicAfter<N>}
//   BootGuard::LimitReaction         Runtime/BootGuardRules.hpp;            OnLimit::{Ignore, Log, Count, Call<&fn>,
//                                    OnLimit::Log                        SafeMode, Panic, PanicAfter<N>,
//                                                                        Halt<Startup, Stop>}
//   ImageCheck::MismatchReaction     Runtime/ImageCheck.hpp;                OnMismatch::{Ignore, Log, Count, Call<&fn>,
//                                    OnMismatch::Log                     Panic, PanicAfter<N, Debugger>}
//   Check::SoftFailureReaction       Runtime/Check.hpp; OnSoftFailure::     OnSoftFailure::{Ignore, Log, Count, Call<&fn>,
//                                    Panic in sanitize, Log elsewhere    Panic, PanicAfter<N>}
//   Crc::ImageEngines                Runtime/CrcImageEngine.hpp;         a set with `For<P, TableSize>`, the chip's
//                                    SoftwareEngines                     (chip_rp_common DmaCrc::Sniffer<Dma, ch>,
//                                                                        chip_atsam_common DmaCrc::Dmac)
//
// uc_log's ComBackend<Tag::User> and LogClock<Tag::User> have the same shape (uc_log's own tag, its own lookup).
//
// A reaction's name means the same in every On<Event>, and the run goes on after each but Panic and Halt:
//   Ignore         nothing of its own (a wait stays bounded; a starved check gets a fresh window)
//   Log            a line (at its error class's level, Runtime/Check.hpp), never a fatal path. Where the component writes
//                  the event's line itself (BootGuard, a soft check) Log adds none - Ignore is the same run there. A
//                  register wait logs from the main loop, never from the wait: OnTimeout::Log stores and counts, and is
//                  also a Hook::MainLoop entry (list it in the Startup list) that writes one warning per batch of new
//                  timeouts - a wait runs before logging works (the clock set-up; a timeout before initMemory() is
//                  cleared with the RAM), in an interrupt, or in a RAM function with the flash unreadable
//   Count          a counter the reaction holds, no line of its own (BootGuard: `limits`, 1 when this run started at
//                  the limit - the guard's record counts the failed runs)
//   Call<&fn>      the application's function, with the event (a panic's and a fault's must not return)
//   Panic          the component's Panic::Cause
//   PanicAfter<N>  Log (or nothing for BootGuard) until the N-th event "in a row", then Panic: a wait - timeouts with
//                  no wait holding between them; an image check - corrupt passes with no clean pass between (never
//                  while a debugger has halting debug); a supervisor - starvations of this run, any check; a soft check
//                  - failures of this run, any check; the boot guard - boots that start at its limit
//   Halt           the outputs safe, then stop (BootGuard; a panic's OnPanic::Halt, a fault's OnFault::Halt)
//   StopFeeding (Health) - the watchdog resets; SafeMode (BootGuard) - a flag the application asks; Unbounded /
//   Report (a wait) - no bound / the caller reads the WaitResult; Default / Wait / Reset<R> (a panic) - its ends.
// Names an event does not have, on purpose: Halt for a wait, an image check, a soft check and a supervisor (it is
// Panic with the image's PanicReaction = OnPanic::Halt, which keeps the panic record; a supervisor's own halt would
// stop the watchdog it exists for); Ignore, Log, Count and PanicAfter for a panic and a fault (their reaction must not
// return: the run has already ended).
//
// The chip package's Startup hooks below are specialised by the chip in its chip/StartUp.hpp: kvasir/StartUp/
// StartUp.hpp does not have to come after the chip's StartUp.hpp.
#include "kvasir/Common/Tags.hpp"   // Tag::User, Port::Late / LateT

#include <cstdint>

namespace Kvasir::Port {
// The chip's whole-chip reboot, announced to the log printer first: `[[noreturn]] void operator()() const`. The chip
// package specialises it for Kvasir::Tag::User next to its Kvasir::Chip::reboot() (chip_rp_common Reboot.hpp:
// the boot ROM's reboot; chip_atsam_common Reboot.hpp, chip_stm32f413 chip/Reboot.hpp: SYSRESETREQ). The SDK names it
// as Port::LateT<Port::Reboot<Tag::User>, Dep>. Declared only: a chip without one has no reboot.
template<typename T, typename... Ts>
struct Reboot;

// Stops the chip's other cores on a fatal path, so none of them turns an output on again after the safe state:
// `void operator()() const`, register writes, any context, the calling core keeps running. The SDK's panic dispatch
// calls it right after the safe state (Runtime/Panic.hpp). Declared only: a chip with one core (or an image that runs
// code on one) has none, and the call compiles away.
template<typename T, typename... Ts>
struct StopOtherCore;

// The image-wide lock the atomic shim (StartUp/Runtime/LibAtomic.hpp) takes on a two-core chip whose cores have no
// exclusive accesses (the Cortex-M0+): `static void acquire()` spins until this core holds it, with a barrier after;
// `static void release()` with a barrier before; register accesses only, any context, interrupts already masked by the
// caller. The shim names it through LateT in its lock's class template (Shim::LockT), whose members are instantiated
// at the end of the TU with main, so the chip's specialisation (chip_rp2040 chip/CrossCoreLock.hpp) may come before or
// after kvasir/StartUp/StartUp.hpp. Declared only: the shim's static_assert names a missing one.
template<typename T, typename... Ts>
struct CrossCoreLock;

// The cause of this boot's reset as the chip names it: `operator()() const`, returning the chip's own enum (a value
// uc_log formats), the value of the chip's Kvasir::PM::resetCause(). The chip package specialises it next to that
// function (chip_rp_common chip_reset.hpp, every SAM and STM32 package's chip/PM.hpp); the
// boot report (kvasir/Runtime/BootReport.hpp) names it as Port::LateT<Port::ResetCause<Tag::User>, Form>, so that
// header may come before the chip's. Declared only: Boot::Report's static_assert names a missing one.
template<typename T, typename... Ts>
struct ResetCause;

// Whether this start is the chip's own reset after the image was entered without one (a debugger's or the image's
// own jump): `bool operator()() const`, valid from main() on. Boot::logBoot (kvasir/Runtime/BootLog.hpp) then adds a
// line after the reset cause. Declared only: a chip whose start code never resets the chip itself has none, and the
// line compiles away (chip_atsam_common StartUp.hpp has one).
template<typename T, typename... Ts>
struct RestartedAfterJump;

// The chip package's own lines about how this start went, for the boot report: `void operator()() const`, logs them
// itself (nothing when there is nothing to say), valid from the runtime inits on. Boot::logBoot calls it after the
// reset cause and the restart line. Declared only: a chip without one adds nothing (chip_rp_common clock_config.hpp:
// a boot that runs degraded on the ring oscillator after failed clock inits, Clocks::DegradedBoot).
template<typename T, typename... Ts>
struct StartNotes;

// An address no bus slave answers at, cited from the chip's memory map: `static constexpr std::uintptr_t value`, a
// word-aligned address whose load or store ends in a bus error (a precise BusFault on Armv7-M/Armv8-M for a load, a
// HardFault on the M0+). Fault::Inject (kvasir/Runtime/FaultInject.hpp) faults there on purpose. The chip package
// specialises it in its chip/StartUp.hpp. Declared only: a chip without one has no busFaultLoad()/busFaultStore().
template<typename T, typename... Ts>
struct UnmappedAddress;

// The chip's free-running microsecond counter, as it reads now: `std::uint64_t operator()() const`, register reads
// only, any context, any core, interrupts masked or not, also on a fatal path before the clocks are complete. The
// panic record keeps the time of the first panic (Panic::FullRecord, Runtime/Panic.hpp) and the next boot's report
// prints it. Declared only: a chip without such a counter records 0 and the report prints no time (chip_rp_common
// FreeRunningUs.hpp: the TIMER, counting since its release in this boot).
template<typename T, typename... Ts>
struct FreeRunningUs;
}   // namespace Kvasir::Port

namespace Kvasir { namespace Startup {
    // The first thing a reset runs after the core's clean-up (resets, clocks the chip needs before anything else).
    // Declared only: Port/Check.hpp names a missing one.
    template<typename T, typename... Ts>
    struct FirstInitStep;

    // The chip's source of per-boot randomness for the stack guard (__stack_chk_guard), read once
    // per boot by core 0 between initMemory() and the global constructors. The default has none
    // and returns the constant 0xdeadc0de: the canary still catches a stray write, it just is not secret.
    // Specialised by the chip layer for Tag::User.
    template<typename T, typename... Ts>
    struct StackGuardEntropy {
        [[gnu::always_inline]] std::uint32_t operator()() const { return 0xdeadc0deU; }
    };

    // RAM the chip's linker script adds beside .data/.bss (chip_rp2040: the SRAM4/5 scratch
    // banks): copied and zeroed by core 0 right after initMemory(), before the global
    // constructors. The default has none. Specialised by the chip layer for Tag::User.
    template<typename T, typename... Ts>
    struct ExtraMemoryInit {
        [[gnu::always_inline]] void operator()() const {}
    };

    // The secondary-core counterpart of FirstInitStep: what a core other than the boot core
    // needs done for itself. Specialised by the chip layer for Tag::User with
    //   static constexpr std::uint32_t cpacrEnable;   // ORed into CPACR by the entry trampoline
    //   void operator()();                             // anything else, from C++
    // Declared only: SecondaryCore names a missing one (Port/Check.hpp).
    template<typename T, typename... Ts>
    struct SecondaryCoreInit;
}}   // namespace Kvasir::Startup

namespace Kvasir::Register {
// The fastest clock the core may run at when a wait runs: `static constexpr std::uint64_t value`, in Hz. The chip
// package specialises it in its chip/WaitBounds.hpp, before anything names a bound. Register/Wait.hpp's
// MicrosecondsBound and Runtime/AnnouncedReset.hpp's poll counts turn microseconds into polls at it, so a slower clock
// only makes the real time longer. The primary's 0 is their named error.
template<typename T, typename... Ts>
struct CpuHzCeiling {
    static constexpr std::uint64_t value = 0;
};
}   // namespace Kvasir::Register
