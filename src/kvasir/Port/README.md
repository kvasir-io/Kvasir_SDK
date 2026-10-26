# kvasir/Port: the port contract

What the SDK needs from the core and chip packages under an image, in one place. The headers include only the
standard library, each other and `kvasir/Common/Tags.hpp`; one exception, `Extends.hpp` (the named hooks), also
includes `kvasir/Mpl/brigand.hpp`, `kvasir/Mpl/List.hpp`, `kvasir/StartUp/Entry.hpp` and `kvasir/Util/Diagnostic.hpp`
(multi-extend lists, the entry tables, its named errors). A missing piece is a named `Kvasir port:` error.

| piece | header | provided by | used by | checked |
|---|---|---|---|---|
| `Startup::FirstInitStep<Tag::User>` | `Hooks.hpp` (declared only) | the chip, `chip/StartUp.hpp` | Startup's reset body, through `Port::LateT` | `Port::ChipFirstInit`, in `Start<>` |
| `Startup::StackGuardEntropy<Tag::User>` | `Hooks.hpp` (default: the constant 0xdeadc0de) | the chip, optional | `seedStackGuard<Dep>()` | - |
| `Startup::ExtraMemoryInit<Tag::User>` | `Hooks.hpp` (default: none) | the chip, optional | the reset body, after `initMemory()` | - |
| `Startup::SecondaryCoreInit<Tag::User>` (`cpacrEnable`, `operator()`, `launch`, `reset`; optional `armCoreResetGuard`, `armOtherCoreStop`, `PrimaryIsr`: core 0's vectors) | `Hooks.hpp` (declared only) | the chip, if it has a second core | `SecondaryCore` | `Port::ChipSecondaryCore`, in `SecondaryCore` |
| `Port::Reboot<Tag::User>` (`[[noreturn]] void operator()() const`) | `Hooks.hpp` (declared only) | the chip, next to `Chip::reboot()` | the SDK's fatal ends, through `Port::LateT` | the chips' conformance trees |
| `Port::ResetCause<Tag::User>` (`operator()() const` -> the chip's reset cause enum) | `Hooks.hpp` (declared only) | the chip, next to `PM::resetCause()` (chip_rp_common `chip_reset.hpp`, each SAM/STM32 `chip/PM.hpp`) | `Boot::Report`, through `Port::LateT` | `Boot::Report`'s `static_assert` |
| `Port::StopOtherCore<Tag::User>` (`void operator()() const`) | `Hooks.hpp` (declared only) | a chip with a second core (chip_rp_common `StopOtherCore.hpp`, multicore images only: core 0 resets core 1, core 1 parks core 0 in its NMI) | the panic dispatch, after the safe state | `kvasir_test_panic_default_end` (a stand-in) |
| `Port::CrossCoreLock<Tag::User>` (`static void acquire()`, `static void release()`) | `Hooks.hpp` (declared only) | a two-core chip whose cores have no exclusive accesses (chip_rp2040 `chip/CrossCoreLock.hpp`, multicore images) | the atomic shim's lock (`StartUp/Runtime/LibAtomic.hpp`), through `Port::LateT` | the shim's `static_assert` |
| `Port::RestartedAfterJump<Tag::User>` (`bool operator()() const`) | `Hooks.hpp` (declared only) | a chip whose start code resets the chip itself (chip_atsam_common `StartUp.hpp`) | `Boot::logBoot`'s "restarted after a jump" line | - |
| `Port::StartNotes<Tag::User>` (`void operator()() const`) | `Hooks.hpp` (declared only) | a chip with lines of its own about the start (chip_rp_common `clock_config.hpp`: a degraded boot, on which tree) | `Boot::logBoot`, after the reset cause | - |
| `Startup::ChipEntries<Tag::User>` (`using type =` a set with `template<typename ClockSettings> using For = brigand::list<...>`) | `StartUp/Entry.hpp` (default: `NoChipEntries`) | a chip with Startup entries of its own in every image (chip_rp_common `DegradedBoot.hpp`: `Clocks::DegradedBoot<ClockSettings>`) | `Startup::Entries`, before the listed entries; listed by hand: `ListRules` ChipEntryNotListed | - |
| `Register::CpuHzCeiling<Tag::User>` (`static constexpr std::uint64_t value`) | `Port/Hooks.hpp` (0: the named error) | the chip, `chip/WaitBounds.hpp` | `MicrosecondsBound<Us>`, AnnouncedReset's poll counts | `kvasir_test_register_wait_must_fail` |
| `Nvic::InterruptOffsetTraits<Tag::User>` | `Interrupts.hpp` (declared only) | the chip, `chip/Interrupt.hpp` (on the core's `Core::InterruptRules`) | Startup's vector table and checks, the core's NVIC actions | `Port::ChipInterrupts`, in `Start<>` |
| `Startup::Core::cleanCore()` | `Core.hpp` (template fallback) | the core, `core/StartUp.hpp` | the reset body | `Port::CoreProvides`, in `Start<>` |
| `Nvic::clearAllEnablesAndPending()` | `Core.hpp` (template fallback) | core_cortex_common `Nvic.hpp` | the reset body | `Port::CoreProvides`, in `Start<>` |
| `PM::resetKind()` -> `ResetKind` | `ResetKind.hpp` (the enum, a template fallback) | the chip's PM | `BootGuard` | `Port::ChipResetKind`, in `BootGuard::Guard` |
| linker symbols, `linkerAddress<>`, `PrimaryStackTop`, `Core1StackTop` | `LinkerSymbols.hpp` | the SDK's linker scripts | Startup, Panic, the fault handler, StackUsage, chips | the link |
| `KVASIR_CORE_*` (0/1) | `Capabilities.hpp` | derived from the compiler's `__ARM_ARCH_*`; each core's `core/Features.hpp` asserts its values | `#if` tests in the SDK | the core's `static_assert`s |
| `Hook::{MainLoop,SafeState,HealthCheck,AllChecksReported}`, `Extend`, `Turn`, the hook profiler, `Startup::Detail` run machinery (`run`, `runTurn`), `Startup::ExtendsOfList`, `Startup::returnsVoidOn` | `Extends.hpp` | the SDK (named hooks a Startup list's entries add functions to) | Startup, Runtime (Executor, LoopProfiler, stores), Health, chips and drivers that list a `turn()` | the SDK tests `hooks_tests`, `executor_tests` |

- A hook is named `Port::LateT<Hook<Tag::User>, Dep>`: the same type, looked up when Dep (the Startup's clock
  settings) is known, so `kvasir/StartUp/StartUp.hpp` may come before the chip's StartUp.hpp. Every hook of the
  SDK, the chip's and the application's (`Panic::PanicReaction`, `Health::StarvationReaction`, ...), has this one
  form; the table of all of them is the header comment of `Hooks.hpp`. `Port::Late`/`LateT` live in
  `kvasir/Common/Tags.hpp` next to `Tag::User`, so the register layer names its hook (`Register::ExecuteSeam`, the
  chip's header in `KVASIR_CHIP_REGISTER_SEAM`, included at the end of `Register/Exec.hpp`) the same way. A defaulted
  `Traits = InterruptOffsetTraits<Tag::User>` template parameter is a late lookup too: the default is read at the use.
- So every SDK header may come before the chip's headers in a TU: each chip's conformance tree compiles every header
  of `src/kvasir` first and `chip/chip.hpp` after it (`tests/conformance/conformance.cmake`; the headers that cannot,
  each with its reason: `tests/conformance/sdk_first_not.txt`).
- Translation units: `kvasir/StartUp/StartUp.hpp` (or `SecondaryCore.hpp`, which includes it) is the header of the
  image's main TU - the one with `main()` and `template struct Kvasir::Startup::Start<Startup>;` - and of no other:
  its `StartUp/Runtime/` fragments define the libc / AEABI / libatomic stand-ins strongly. A main names it itself, in
  any order with its chip and board headers; `chip/chip.hpp` does not bring it. Every other header (the chip's, the
  drivers', the SDK's) is TU-safe: no strong definition, the chip's per-image data (`.boot2`, the RP2350's IMAGE_DEF)
  `inline`. A second TU with StartUp.hpp fails the link on the symbol `StartUp/Runtime/All.hpp` defines for it,
  `kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image`. A firmware header that needs Startup machinery and may be
  in another TU includes the piece (`StartUp/Entry.hpp`, `Port/Extends.hpp`, `StartUp/Resources.hpp`).
- Not hooks, kept as they are: the three template fallbacks of the table (no choice: the core's or chip's own function,
  which firmwares also call, `PM::resetKind()`; the fallback only names a missing piece) and the weak
  `extern "C" kvasir_announced_reset_time_us()` (a link-level contract between the chip and uc_log, which builds
  without the SDK's headers).
- A fallback is a template that a core's or chip's own non-template function beats in overload resolution. The
  caller passes `decltype` of the call, taken where it is written: StartUp.hpp includes the core headers first.
- The contract has no version number: a core or chip package is built with the SDK of the same state (master); a
  piece that changes changes in the SDK and in every package in the same change.
- Tests: `tests/port_tests.cpp`, `tests/port_must_fail/`.

