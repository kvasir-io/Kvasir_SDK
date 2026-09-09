# Two cores

Kvasir runs a second core the same way it runs the first: from a Startup list. This
document is the model, the rules, and the primitives. The reference implementation is the
RP2350; the SDK side is chip-agnostic.

## Opt-in, and what a single-core build sees

Nothing here exists in a build that does not ask for it. Passing `CORE1_STACK_SIZE` to
`kvasir_executable_variants` (or `target_configure_kvasir`) is the switch:

```cmake
kvasir_executable_variants("" SOURCES src/main.cpp CORE1_STACK_SIZE 16k)
```

It sizes the linker's `.stack1` section and defines `KVASIR_MULTICORE=1`. Without it the
image is what it was before multicore support existed, byte for byte: an empty `.stack1`
with two symbols, no define, the interrupt-mask atomic shim, single-core queue ordering.
Everything that changes generated code is gated on the define or is a default-off policy;
new functionality lives in new headers that emit nothing unless instantiated.

## The model

- **One Startup list per core.** `Kvasir::Startup::Startup<ClockSettings, Peripherals...>`
  is core 0's, as always. `Kvasir::Startup::SecondaryCore<&core1Main, Peripherals...>` is
  core 1's. A peripheral listed for core 1 gets all of its init steps run on core 1, in the
  same order core 0 would run them, and its ISRs go into core 1's vector table.
- **A peripheral belongs to exactly one core.** Its interrupt is enabled in that core's
  NVIC and nowhere else. `Startup` static_asserts that no type appears in both lists, that
  no two peripherals in one list claim the same interrupt vector, that no chip-wide vector
  is in both tables, that a peripheral which names its core is on it, and that a driver
  and the hardware it depends on are on the same core (see "Resources" below).
- **Core 0 launches core 1 from its Startup list.** Put the `SecondaryCore` last in core
  0's list; its `runtimeInit` launches core 1 after core 0's own list has completed, and
  blocks until core 1's list has completed too. By the time `main()` runs, both cores are
  configured. `Startup` static_asserts that nothing with a `runtimeInit` follows the
  `SecondaryCore`.
- **Chip-wide init happens once, on core 0.** Clocks, resets, `.data`/`.bss`, constructors.
  Core 1 does its own `earlyInit` (fault enables are SCB state, per core), then the
  register phases, then `Nvic::enable_all()`, then its runtime inits, then `core1Main`.

```cpp
[[noreturn]] void core1Main();

using Core1FaultHandler = Kvasir::Fault::Handler<Kvasir::Fault::CleanUpActionNone,
                                                 Kvasir::Fault::FaultActionAssert,
                                                 4096,                              // < CORE1_STACK_SIZE
                                                 Kvasir::Startup::Core1StackTop>;

using Core1 = Kvasir::Startup::SecondaryCore<&core1Main, Core1FaultHandler, Dma1, DisplayQspi>;

using Startup = Kvasir::Startup::Startup<HW::ClockSettings, Clock, HW::ComBackend,
                                         FaultHandler, StackProtector, HW::PinConfig,
                                         Core1>;
```

`Core1::launch()`, `reset()`, `running()`, `stackHighWater()` and `stackSize()` are
available for applications that want manual control; not listing `Core1` in `Startup` and
calling `launch()` from `main()` is equally valid.

`launch()` waits for core 1 to report ready. List a `LaunchTimeout<Clock, ms>` in the
`SecondaryCore` list to bound that wait on a clock core 0 can read; without one it is a
fixed number of polls, which is a time only for one core clock.

### Resetting and relaunching

`reset()` puts core 1 back into the bootrom's holding pen; `launch()` brings it back
through its Startup list and `core1Main`. Whatever core 1 was holding when it died goes
with it, and nothing but the reset itself undoes that:

- the SDK releases its own atomic-shim lock (the one behind 8-byte atomics and CAS) in
  `reset()`, otherwise the next such operation on core 0 would spin on it forever;
- every peripheral in the `SecondaryCore` list gets its `primaryPrepare()`; `TaskQueue`
  and `ClockSync` implement it, which is why a `TaskQueue` belongs in that list even
  though it has no init steps;
- everything else is the application's: a `Spinlock` core 1 held (nothing can release it
  safely, it is simply gone with the core), a `Seqlock` core 1 was writing (`reset()` it,
  or its counter stays odd and readers never see a snapshot again), a DMA channel core 1
  started, the `static` state of any driver core 1 owned. Give those a Startup-list entry
  with a `primaryPrepare()` and list it in the `SecondaryCore`.

`launch()` after `main()` also re-runs core 1's register phases: `RESETS`, `IO_BANK0` /
`PADS`, `PIO`, DMA `INTE`. Those are read-modify-writes; core 0 must not be touching those
blocks while it happens (rule 2 below applies in both directions at that moment).

### Rebooting the chip

`Kvasir::SystemControl::SystemReset` (AIRCR.SYSRESETREQ) is not a chip reset on the RP2040
and RP2350: it warm-resets the core that writes it and nothing else. From core 1 that parks
core 1 in the bootrom's holding pen while core 0 keeps running against a partner that is
gone (measured on the RP2350: core 1 at the bootrom's `wait_for_vector`, core 0 still in
`main()`); from core 0 it leaves core 1 and every peripheral running. A reboot that restarts
both cores and the peripherals from the bootrom is `Kvasir::reboot()` in the chip layer
(`chip/rp_common/bootrom_functions.hpp`), which goes through the watchdog: on the RP2350 the
bootrom's `reboot()` arms the watchdog timer and `PM::reset_cause()` reports `watchdog_timer`,
on the RP2040 the raw trigger reports `watchdog_force`.

## Resources

`kvasir/StartUp/Resources.hpp`. A peripheral may say what hardware it configures and what
hardware it uses:

```cpp
using Provides = brigand::list<Resource<Tag, Id...>...>;   // a DmaBase: its channels, its line
using Claims   = brigand::list<Resource<Tag, Id...>...>;   // a driver: the channels it starts
```

and `Startup` derives one more kind of provide from what a peripheral *does*: every action
in its `initStepPinConfig` goes through `ResourceOfAction`, which the chip layer
specialises for the register write that means "this peripheral now owns GPIO n"
(IO_BANK0's `GPIOn_CTRL` on the RP2350). A pin is therefore provided by whoever configures
it, with no declaration anywhere.

`Startup` collects all of it from its own list, from every `SecondaryCore` in it, and from
`ClockSettings`, and refuses to build unless

1. no resource is claimed twice (two drivers on one DMA channel overwrite each other's
   callback and registers);
2. no resource is provided twice (two `DmaBase` instances on one interrupt line clobber
   each other's `INTE` mask; two peripherals configuring one pin; two PIO programs on one
   state machine or overlapping in instruction memory; two drivers on one SPI instance);
3. every claim is provided somewhere (a `DmaBase` in no Startup list never enables its
   line; a pin handed to a driver that nobody configures stays isolated; a driver's
   `clockSpeed` that is not the clock it runs from);
4. the provider is in the same core's list as the claimer (a `DmaBase` on core 1 runs its
   completion callbacks on core 1, so a driver on core 0 using one of its channels has its
   callback on the wrong core);
5. a peripheral that names its core (`static constexpr unsigned startupCore = N;`) is in
   core N's list (a `GpioIrqOn<1, ...>` in core 0's list enables the mask in one core and
   the NVIC line in the other).

Each rule has its own `static_assert` message. Resources are keyed by the hardware (channel
2, GPIO 19, slice 3), not by the type that provides it, which is what makes rule 2 catch two
peripherals that overlap without either knowing about the other. Not every kind of
resource wants every rule, so a tag may carry policies: `sharedClaim` (several users of one
input are fine), `coreLocal = false` (a pad is chip-wide, so the provider may be on either
core), `mergeIdentical` (two providers of one PWM slice are one provider if they agree on
its counter), `optionalProvider` (a claim is only checked once something provides the tag,
so an application that has not declared its clocks stays unchecked rather than broken) and
`keyArity` (a GPIO is `(port, pin)`).

Claims only count for types in a Startup list. A driver that is not a peripheral (a display
panel, a network controller) but is handed pins is listed for its claims alone; it has no
init steps, so listing it costs nothing.

What the RP2350 chip layer declares today:

| Resource | Provided by | Claimed by |
|---|---|---|
| DMA channel, DMA interrupt line | `DmaBase` (from its config) | every DMA-using driver, `Kvasir::DMA::Claims<Dma, Channel...>` |
| GPIO | whoever writes its `GPIOn_CTRL` in `initStepPinConfig` | `GpioIrq`, `Io::PinClaims<Pins...>` in drivers handed a pin |
| PIO state machine, instruction slot | `Kvasir::Pio::Provides<Instance, Sm, Offset, Program>` in `Pio::StateMachine`, `WS2812`, `PioQspi`, `Cyw43::PioSpi` | |
| SPI, I2C, UART instance; the ADC, USB, watchdog, a TIMER | their base classes | |
| PWM output; PWM slice with its `(div, top)` | `PWM<Pin>`, `PWM_Timer<Slice>` | |
| `clk_sys`, `clk_peri`, `clk_ref`, `clk_adc`, `clk_usb`, the processor clock | `ClockSettings::Provides = DefaultClockSettings::Provides<ClockSpeed, CrystalSpeed>` | every driver, at its config's `clockSpeed` (`Clocks.hpp` says which block counts which clock) |

Next to the resources, `Startup` refuses an interrupt vector installed in both cores'
tables unless the chip lists the line as per core (`InterruptOffsetTraits::perCore`: SIO,
IO_BANK0 and the core exceptions on the RP2350). A chip-wide line enabled in both NVICs
runs its ISR on both cores at once.

## Posting work to the other core

`Kvasir::Multicore::TaskQueue<Config>` (`kvasir/Multicore/TaskQueue.hpp`): post a lambda on
one core, run it on the other, get the result through a future. Fixed slots, 8-byte
aligned capture and result storage, no heap.

```cpp
struct Core1TasksConfig {
    static constexpr std::size_t slots       = 8;    // in flight
    static constexpr std::size_t captureSize = 32;   // bytes of captures per task
    static constexpr std::size_t resultSize  = 16;   // bytes of result
};
using Core1Tasks = Kvasir::Multicore::TaskQueue<Core1TasksConfig>;

auto f = Core1Tasks::post([a, b] { return crc(a, b); });   // core 0
auto v = f.get();                                          // sleeps in wfe() until done
f.waitUntil(Clock::now() + 5ms);                           // bounded
Core1Tasks::post([] { blink(); });                         // fire and forget: it still runs

Core1Tasks::run();      // core 1: dedicated worker, or
Core1Tasks::poll();     // core 1: one task per turn of your own loop
```

Captures and results must be trivially copyable and trivially destructible: pass indices
and pointers to static objects. Mutable lambdas are fine. One producer core and one
consumer core per instance; the other direction is a second instance. Posting from an ISR
too needs a `CriticalSection` around `post()`. A full queue returns an invalid future after
running `Config::OverflowPolicy` (assert by default). Measured on the RP2350 with core 1
polling from its render loop: about 1 us post-to-get including the wait for the strip in
progress; in a tight loop the queue round trip is well under a microsecond.

## SysTick on both cores, synchronised

SysTick is per core. Two distinct config types give two clocks with their own counter and
ISR; each is read only on its own core (the registers are banked at the same address, so
reading one core's SysTick clock from the other silently mixes the counters). Both count
`clk_sys`, so they never drift against each other: one rendezvous at launch fixes the
start offset for good.

```cpp
struct Core1SystickConfig : Core0SystickConfig { static constexpr bool synchronised = true; };
using Core1Systick = Kvasir::Systick::SystickClockBase<Core1SystickConfig>;
using SystickSync  = Kvasir::Multicore::ClockSync<Core0Systick, Core1Systick>;

using Core1   = SecondaryCore<&core1Main, Core1Systick, SystickSync, ...>;
using Startup = Startup<..., Core0Systick, Core1>;   // Core0Systick before Core1
```

`ClockSync` uses the `SecondaryCore` rendezvous hooks: core 1 announces itself from its
Startup list, core 0 (still inside `launch()`) reads its clock, raises a flag, core 1 reads
its raw counter and answers, core 0 reads its clock again. Core 1's reading lies between
the two, so it is paired with their midpoint, right to within half the bracket; eight
rounds, the tightest bracket wins. Repeating matters: both cores execute from the same
flash through one XIP cache, and a cold fetch between flag and read costs microseconds
(one round measured 1.7 us off, the best of eight is a few tens of nanoseconds). Measured
on the RP2350: offset 35-55 ns, bracket +/-400 ns, and no drift over a minute. Both sides
are bounded, and `launch()` returns `false` if the rendezvous fails.

When to use which: `TimerClockBase` (TIMER0) is one counter for the chip, 1 us, no ISR,
nothing to sync. Synchronised SysTicks give 5 ns resolution on each core at the cost of
one ISR per core and the launch rendezvous.

### Rendezvous hooks

A peripheral in a `SecondaryCore` list may define `static void primaryPrepare()` (core 0,
before the bootrom handshake: reset shared state), `static bool primarySync()` (core 0,
after the handshake, before the ready wait) and `static bool secondarySync()` (core 1,
after its runtime inits, before it reports ready). The two sync sides run concurrently and
may wait on each other; each must bound its own wait. `SecondaryCore::synchronised()` says
whether every secondary side met its counterpart.

## Rules

1. **Own, do not share.** Two cores may not drive the same peripheral. `Nvic::InterruptGuard`
   masks this core's interrupts and says nothing to the other core, and every existing
   driver relies on it; the split has to be at the peripheral, not inside it. Enforced
   where the chip layer declares it (see "Resources"): a driver and its `DmaBase` must be
   in the same core's list, two instances may not share a channel or an interrupt line, a
   chip-wide interrupt may not be in both vector tables, and a pin, a PIO state machine or
   a peripheral instance has one owner.
2. **No chip-wide configuration from core 0 after launch.** Register writes are
   read-modify-write. Core 1 runs its peripherals' init steps (`RESETS`, `IO_BANK0`, PIO
   configuration) once, during `launch()`, while core 0 waits; after that, core 0 must not
   touch those blocks.
3. **Data shared between cores goes through `Kvasir::Atomic`.** `Seqlock<T>` for "the
   latest value", `Queue<T, N>` for a stream (cross-core ordering is the default in a
   multicore build), `Spinlock` for a short critical section, `CriticalSection` when an ISR
   on either core is involved. `std::atomic<T>` of 1, 2 and 4 bytes is inline and correct;
   8 bytes and CAS go through the shim, which takes a spinlock in a multicore build.
   **Never wait unbounded on the other core.** A core can halt at any instruction (fault,
   breakpoint, a debugger halting just that core, a reset); a reader spinning on its
   `Seqlock` or FIFO hangs with it, and usually inside the code meant to report the stall.
   Use `Seqlock::tryRead(out, attempts)` and keep the last snapshot, `Fifo::read(deadline)`,
   `Future::waitUntil()`.
   A queue whose two ends are one core's thread and ISR (a driver's command queue) should
   name `Atomic::SyncSignal` explicitly: the multicore default costs a `dmb` per push and
   pop, measured at about 2 Mbit/s of UDP throughput on the W5500 driver's per-frame path.
4. **Time comes from a chip-level timer.** SysTick is per core. `Kvasir::Timer::TimerClockBase`
   (RP2350 TIMER0) reads the same on both cores and needs no interrupt.
5. **Logging is per core.** `uc_log::MulticoreRttComBackend` gives every (core, context)
   its own RTT up buffer; the printer shows the buffer index per line (2 and 3 are core 1).
6. **Flash writes need core 1 parked.** Not implemented; if you write flash, `reset()`
   core 1 first and `launch()` it afterwards.
7. **Faults on core 1 need their own handler**, with `Startup::Core1StackTop` and a reserve
   smaller than `CORE1_STACK_SIZE`. MSPLIM is armed on entry: a core 1 stack overflow is a
   `StackOverflow` fault on core 1 and nothing on core 0.
8. **`KVASIR_MULTICORE` must reach every translation unit that includes a Kvasir header.**
   It changes `Atomic::Queue`'s default ordering and the atomic shim's lock; a static
   library compiled without it and linked into a multicore image would disagree with the
   application about both. `kvasir_executable_variants` defines it on the executable; a
   library of your own that includes Kvasir headers needs it too.

## Primitives

| | |
|---|---|
| `Kvasir::Core::{dmb, dsb, isb, sev, wfe, wfi}` | the barriers and events, with memory clobbers |
| `Kvasir::Atomic::Spinlock` | Lockable, on exclusive accesses |
| `Kvasir::Atomic::CriticalSection` | Spinlock plus this core's interrupt mask |
| `Kvasir::Atomic::Seqlock<T>` | one writer, any readers, writer never waits |
| `Kvasir::Atomic::Queue<T, N, Overflow, Sync>` | SPSC ring; `SyncThread` default when multicore |
| `Kvasir::Sio::cpuId()`, `Sio::CpuId` | which core |
| `Kvasir::Sio::Fifo` | the 8-deep mailbox, both directions |
| `Kvasir::Sio::Doorbell<N>` | ring the other core; an interrupt there with `DoorbellIsr` (RP2350) |
| `Kvasir::Sio::FifoIsrOn<Core, F>`, `FifoIsr<F>`, `DoorbellIsr<F, N...>` | Startup-list peripherals claiming the SIO interrupts |
| `Kvasir::Multicore::launchCore1 / resetCore1` | the chip half of `SecondaryCore` |
| `Kvasir::Timer::TimerClockBase<Config>` | the shared clock |

### Exclusives need ACTLR.EXTEXCLALL

A Cortex-M33 sends an exclusive access to the bus fabric's global monitor only for
shareable memory; by default nothing is shareable, so `ldrex`/`strex` from the two cores
never arbitrate and both `strex` succeed. The chip's `FirstInitStep` (core 0, gated on
`KVASIR_MULTICORE`) and `SecondaryCoreInit` (core 1) set `ACTLR.EXTEXCLALL`, which routes
every exclusive to the global monitor. The self-test in `defender_display` measured the
difference: without the bit, a 32-bit `fetch_add` from both cores keeps 100 002 of
200 000 increments; with it, all of them.

### Why software spinlocks

The RP2350 has 32 hardware spinlocks in SIO. Erratum RP2350-E2: a write to any SIO
register above +0x180 (doorbells, MTIME, TMDS, PERI_NONSEC) aliases the spinlocks and
releases whichever is held. pico-sdk defaults to software locks on the RP2350 for this
reason and so does Kvasir; the hardware locks are not wrapped. Exclusive accesses
(`ldaexb`/`strexb`) are coherent across the RP2350's two cores, which is what
`Atomic::Spinlock` and the atomic shim's lock are built on.

### An exclusive store sets this core's own event

Armv8-M raises an event whenever the global monitor leaves the Exclusive state
(DDI0553 B9.3.1), and the RP2350 makes a core's *own* successful `strex` do that. So every
`std::atomic` read-modify-write or `compare_exchange`, every `Spinlock` or
`CriticalSection` acquire, and every 8-byte atomic (they go through the shim's
`ldaexb`/`strexb` lock) leaves the event register set on the core that did it, and a
`wfe()` right after returns at once. pico-sdk 2.3.1 found its own primitives busy-waiting
because of this (`PICO_EXCLUSIVE_ACCESS_SETS_OWN_EVENT`).

The rule: the check in front of a `wfe()` loop is a plain load or a register read.

```cpp
while(!ready.load(std::memory_order_acquire)) { Core::wfe(); }   // sleeps: a load
while(!Sio::Fifo::readable())                 { Core::wfe(); }   // sleeps: a register read

while(!lock.try_lock())                       { Core::wfe(); }   // spins: strex each turn
while(!done64.load())                         { Core::wfe(); }   // spins: 8 bytes -> ShimLock
while(counter.fetch_add(0) == 0)              { Core::wfe(); }   // spins: an RMW
```

`TaskQueue::Future::wait()` and `TaskQueue::run()` are the built-in examples of the first
shape. An exclusive *inside* the loop body is fine as long as it is followed by another
plain check before the next `wfe()`; it costs one extra pass, not the sleep.

## The RP2040

The same model on the Cortex-M0+ pair, with three differences the chip layer absorbs:

- **No exclusive accesses.** Every `std::atomic` read-modify-write, `Spinlock` and
  `CriticalSection` goes through the atomic shim, whose cross-core lock is the SIO's
  `SPINLOCK31` (`chip/CrossCoreLock.hpp` in the chip package; no erratum touches the
  RP2040's spinlocks). Correct, and a few times slower than the RP2350's inline exclusives;
  plain loads and stores of 1, 2 and 4 bytes stay inline. No `ACTLR.EXTEXCLALL`, and no
  exclusive-sets-own-event trap.
- **No stack limit register.** A core 1 stack overflow is not caught; `stackHighWater()` is
  what says how close it came. The fault handler on `Core1StackTop` has nothing to clear,
  so its entry points are the primary ones.
- **No doorbells, and one FIFO interrupt line per core.** The mailbox FIFO is the wake-up:
  core 1 writes a word, core 0 takes `FifoIsr<F>` (`sio_proc0`); an ISR for core 1's inbox
  is `FifoIsrOn<1, F>` (`sio_proc1`), and Startup refuses it in the other core's list. The
  launch handshake is the same bootrom protocol as on the RP2350.

`examples/14_multicore` in test_examples builds for both chips: a doorbell on the RP2350,
the FIFO on the RP2040.

## Debugging

The J-Link device name `RP2350_M33_0` attaches to core 0; `RP2350_M33_1` to core 1. RTT is
memory, so the printer on core 0 sees core 1's buffers. A core 1 fault logs through core 1's
own buffers with `COREFAULT`; a lockup on core 1 (a fault inside the fault handler) is
silent, which is what the MSPLIM clear in the secondary fault handler and the reserve
smaller than the stack are for.

`SecondaryCore::launch()` is bounded: a core 1 that does not answer the handshake returns
`false` rather than hanging core 0. The usual reason is a debugger that restarted core 0
alone while core 1 kept running old code; `launch()` resets core 1 and retries once.
