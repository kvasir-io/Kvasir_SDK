# kvasir/Runtime: what an image needs to survive and report

The runtime model of a Kvasir image: how a failure is classed and what each class does, the one fatal path, what
the next boot reports, how an image is configured, and the verbs its parts share. The headers are the reference;
this file says how they fit together.

## The escalation ladder

Every condition a component meets is one of four classes (the table in `Check.hpp`'s header; the SDK, the core and
chip packages and the libraries above them follow it):

| class | what | the component does | log |
|---|---|---|---|
| P | a programming error: the code broke its own contract | `KVASIR_CHECK*` / `KVASIR_DCHECK*` / `assert`: a panic | the check's line |
| E | the environment misbehaved, the component recovers by itself (a NACK, a timeout, a refused write, a CRC) | a return value (`Kvasir::Result`, or a bool with a reason) or a count; never a panic | at most a rate-limited warning |
| L | a function is lost for longer than its budget (a part absent, a store off, an image corrupt but running) | keeps trying; the product decides through `kvasir/Health/` | an error, once per transition |
| F | going on is unsafe | a `Panic::Cause`, the fatal path below | critical, only after `Panic::beginFatal()` |

Every E and L state recovers by itself when its cause is gone: no state waits for a person, a button or a power
cycle. `KVASIR_SOFT_CHECK` sits between P and E: a failed one logs and returns false, and the image's
`Check::SoftFailureReaction` decides (`Log` by default, `Panic` in the sanitize variant).
`tools/check_fatal_sites.py` enforces "critical only after `beginFatal()` in the same function".

## The fatal path

One end for every fatal condition (`Panic.hpp`): an assert, abort/exit, a smashed stack (`StackProtector.hpp`), a
fatal sanitizer report (`ubsan.hpp`), an unhandled interrupt, a fault (`FaultHandler.hpp`, `FaultReaction.hpp`), a
failed check, a component's own cause (a boot loop, a starved health check, an image CRC mismatch, a register wait
that timed out). Each path runs, in this order:

1. `Panic::beginFatal()` - the image's `FatalStartReaction` (its safe state: `StartUp/SafeState.hpp`, register writes
   only), then the log switches to waiting for room so the fatal line is not dropped;
2. its log line (critical);
3. `Panic::raise(cause)`: interrupts masked, the safe state again, the other core stopped (`Port::StopOtherCore`),
   then the image's `PanicReaction`. Every stock reaction writes the record first (`Panic::FullRecord` in `.noInit`):
   `OnPanic::Default` then reboots through the chip's `Port::Reboot` (announced to a log printer, bounded waits only);
   `Halt`, `Wait`, `Reset<R>` or the application's own are the others.

`FaultInject.hpp` faults on purpose (examples, bench hooks); `Check.hpp` holds the checks and their levels.

## Records and the boot report

A crash leaves one record shape per kind (`FaultRecord.hpp`, `Panic.hpp`, `CrashRecord.hpp`): `Fault::FullRecord`
(the stacked frame, the fault status registers, the stack pointers) and `Panic::FullRecord` (cause, core, site,
detail, time), each a CRC-checked `Persistent` in `.noInit`. `takeLastFault()` / `takeLastPanic()` and
`Boot::Previous{fault, panic}` hold them; `Fault::logFault` / `Panic::logPanic` (`CrashLog.hpp`) print them. An image
may pick another record format through the hook `CrashRecord::RecordFormat`.

Every image says first thing what the run before it ended in: `Boot::Report<>` (`BootReport.hpp`, `BootLog.hpp`) is a
Startup entry whose `runtimeInit` logs the version, the reset cause (the chip's, through `Port::ResetCause`), the
chip's own start notes (`Port::StartNotes`) and the records it takes; `Boot::ReportInMain` is the marker for a main
that reads the records itself. A Startup list with the log backend and no report does not compile.

The boot-loop guard (`BootGuard.hpp`, its pure table `BootGuardRules.hpp`) is listed before the report: it reads the
records and `PM::resetKind()` without taking them, counts the failed runs in a row across resets, and runs the image's
`LimitReaction` at its limit. A deliberate reset calls `plannedReset()` first. A reset of the cores alone (a debugger's
reset or flash) is the chip's `softwareRequest`, also right after a watchdog reset, and never counts.

`Standard.hpp` (`Runtime::Standard<Clock>`) is the usual bundle as one group entry: the fault handler, the stack
protector, a boot guard and the report.

## Configuration channels

An image is configured through four channels, and nothing else:

| channel | what it chooses | example |
|---|---|---|
| the Startup list's entries | which parts run, in which order; each entry's `Provides`/`Claims` are checked against the others | `Startup::Startup<Clocks, Backend, Guard, Boot::Report<>, Driver, Health::Service<>>` |
| late hooks `X<Tag::User>` | one choice for the whole image: a reaction (`On<Event>::...`), an engine, a chip fact; the table is `Port/Hooks.hpp`'s header | `template<> struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> { using type = OnPanic::Halt; };` |
| Config structs | a part's knobs, each optional with its default, read with `KVASIR_CONFIG_OR` (`Util/Config.hpp`); a defaults struct to derive from where a part has many | `struct GuardConfig { static constexpr std::uint16_t limit = 3; };` |
| compile definitions of a variant | what differs between `debug`, `release`, `release_log`, `sanitize`: check levels, the log, sanitizers (`kvasir_executable_variants`) | `KVASIR_CHECK_LEVEL`, `KVASIR_SOFT_CHECK_ESCALATE` |

A hook is named by the SDK through `Port::LateT<Hook<Tag::User>, Dep>`, so its specialisation may come after the SDK
header and before the image's first use of it (`Port/README.md`).

## The verbs

| verb | meaning |
|---|---|
| `runtimeInit()` | an entry's start, after every entry's register phases, in list order, before `main()` |
| `turn()` | one pass of an entry's work from the main loop (`Hook::MainLoop`); it never waits. Returning `Kvasir::Turn` (or bool) says whether there is more to do, so a sleeping loop (`Executor.hpp`) knows when it may sleep |
| `link()` | where a component is with the part or peer it talks to (`Util/Link.hpp`: starting, answering, absent, offline) |
| `logReport()` | a component's boot summary, at boot only |

`Startup::run<Hook>()` and `runTurn<Hook>()` call every entry's function for a hook (`Port/Extends.hpp`, one
dispatch for both).

## Observability

A component's health is its `link()`; `Health::LinkCheck` turns it into a check the supervisor watches, without glue
in main. Counters are named nouns a reader can ask for (a USART's `txTimeouts()`, a supervisor's `recoveryOverruns()`), not
log lines.
A component logs a transition once (L), a recovered event at most rate-limited (E), and its summary in
`logReport()` at boot only. Nothing logs per turn.

## The other blocks

`ImageCheck.hpp` (the image's own flash CRC against the post-link descriptor), the RAM-copy stores (`Store.hpp`: one
retry surface; `CellStore.hpp`: power-cut-safe cells on a `Hal::NorRegion`), `AnnouncedReset.hpp` (a reset the log
printer is told about first), `Executor.hpp` (a main loop that sleeps until its next timer), `LoopProfiler.hpp`,
`StackUsage.hpp`, `CrcImageEngine.hpp` (the image's CRC engine set, the hook `Crc::ImageEngines`). The health
supervisor is `kvasir/Health/`.

## Where a header goes

Firmware and drivers include these; base headers (`kvasir/Util/`) never do. A Runtime header may include
`kvasir/Util`, `kvasir/Mpl`, `kvasir/Common`, `kvasir/Port`, `kvasir/Atomic`, `kvasir/Register`, `kvasir/Hal` (the flash
region of CellStore), `kvasir/StartUp` (the safe state), `kvasir/Health` (its records, for BootGuard), the core package
and uc_log. A new header takes the layer of what it needs (`tools/layers.toml`: base, register, port, core, runtime,
...): a block that needs a hook, a panic or the log is runtime, one that needs only the standard library is a Util
base header. `just layers` checks it.
