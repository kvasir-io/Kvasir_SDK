#pragma once

#include "kvasir/Atomic/Atomic.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/List.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/LinkerSymbols.hpp"
#include "kvasir/StartUp/StartUp.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace Kvasir::Startup {

#if defined(KVASIR_MULTICORE) && KVASIR_MULTICORE
inline constexpr bool multicoreEnabled = true;
#else
inline constexpr bool multicoreEnabled = false;
#endif

// How long launch() waits for the other core to report ready, on a clock the primary core
// can read. Listed in the SecondaryCore's list like a peripheral (it has no init steps):
//
//   using Core1 = SecondaryCore<&core1Main, LaunchTimeout<Clock, 2000>, ...>;
//
// Without one the wait is a fixed number of polls (readyBudget), which is a time only for
// one core clock: a longer core 1 init list or a slower clock then fails launch for no
// reason, so a real application should name its clock.
template<typename Clock, std::uint32_t TimeoutMs>
struct LaunchTimeout {
    using clock                           = Clock;
    static constexpr auto timeout         = std::chrono::milliseconds{TimeoutMs};
    static constexpr bool isLaunchTimeout = true;

    // Startup: the clock is read on core 0, so it has to be in core 0's list, before the
    // SecondaryCore (a core 1 SysTick named here would be core 0's own, silently).
    using Claims = brigand::list<ListedOn<Clock, 0>>;
};

// Listed in a SecondaryCore's list: no CoreResetGuard for this image. By default a chip that has one (RP2040,
// RP2350: chip/CoreResetGuard.hpp) arms it whenever a SecondaryCore is launched - a reset of core 0 alone
// (AIRCR.SYSRESETREQ: the image's, third-party code's, a debugger's) then stops core 1 before the boot ROM touches
// the flash interface core 1 is reading through.
//
//   using Core1 = SecondaryCore<&core1Main, LaunchTimeout<Clock, 2000>, NoCoreResetGuard, ...>;
struct NoCoreResetGuard {
    static constexpr bool isStartupEntry   = true;
    static constexpr bool noCoreResetGuard = true;
};

// The second core's Startup list.
//
//   [[noreturn]] void core1Main();
//   using Core1 = Kvasir::Startup::SecondaryCore<&core1Main, Dma1, DisplayQspi, Core1FaultHandler>;
//   using Startup = Kvasir::Startup::Startup<ClockSettings, Clock, ..., Core1>;   // Core1 last
//
// Every peripheral listed here belongs to core 1: its register init steps, its interrupt
// configuration and its runtime inits run on core 1, in the same order Startup runs them on
// core 0, and its ISRs go into core 1's vector table. What does not run here is everything
// that happens once per chip - clock init, .data/.bss, constructors - because core 0 did
// that before this core existed. The one exception is earlyInit, which is SCB state (fault
// enables) and therefore per core.
//
// Listing the SecondaryCore in the primary Startup list launches it from runtimeInit, i.e.
// after core 0's own list has completed and just before main(); launch() blocks until core
// 1's list has completed too, so when main() runs, both cores are configured. The rules over
// both lists (no peripheral in both, nothing with a runtimeInit after the SecondaryCore, ...)
// are Checks.hpp's ImageChecks, asserted by Startup.
//
// The stack is the linker's .stack1, sized by CORE1_STACK_SIZE (kvasir_executable_variants),
// which also defines KVASIR_MULTICORE and with it the cross-core atomic shim and queue
// defaults. On Armv8-M mainline (the RP2350) MSPLIM is armed at entry, so an overflow is a
// StackOverflow fault on core 1 and nothing on core 0; a Cortex-M0+ (the RP2040) has no
// stack limit, and an overflow there runs into core 0's stack top unnoticed. The stack is
// filled with a pattern at launch, and stackHighWater() reads how much of it was ever used.
//
// The chip half (SecondaryCoreInit<Tag::User>: the CPACR mask, launch(), reset(), optionally armCoreResetGuard(),
// armOtherCoreStop() and PrimaryIsr) lives in
// the chip's StartUp.hpp, next to FirstInitStep.
namespace Detail {
    template<typename List>
    inline constexpr bool anyNoCoreResetGuard = false;

    template<typename... Ts>
    inline constexpr bool anyNoCoreResetGuard<brigand::list<Ts...>>
      = (requires { Ts::noCoreResetGuard; } || ...);

    template<typename Init>
    struct PrimaryIsrOf {
        using type = brigand::list<>;
    };

    template<typename Init>
        requires requires { typename Init::PrimaryIsr; }
    struct PrimaryIsrOf<Init> {
        using type = typename Init::PrimaryIsr;
    };

    template<typename... Ts>
    using FirstLaunchTimeout = MPL::FirstWhereT<ListRules::Detail::IsLaunchTimeout, Ts...>;

    // The hooks and the early phase over the list as this core runs it (SecondaryCore::Entries: its groups replaced by
    // their members, Entry.hpp), as the pack calls Startup's StartupImpl makes over its list.
    template<typename List>
    struct OnEntries;

    template<typename... Es>
    struct OnEntries<brigand::list<Es...>> {
        template<typename H>
        [[gnu::always_inline]] static void callEach() {
            Detail::callEach<H, Es...>();
        }

        template<typename H>
        [[gnu::always_inline]] static bool allTrue() {
            return Detail::allTrue<H, Es...>();
        }

        template<typename P>
        [[gnu::always_inline]] static void applyPhase() {
            Detail::applyPhase<P, Es...>();
        }
    };
}   // namespace Detail

template<void (*Main)(), typename... Peripherals>
struct SecondaryCore {
    static_assert(sizeof...(Peripherals) >= 0 && multicoreEnabled,
                  "SecondaryCore needs a multicore build: pass CORE1_STACK_SIZE to "
                  "kvasir_executable_variants / target_configure_kvasir");

    // The list as this core runs it: every group entry (Entry.hpp) replaced by its members in its place. Everything
    // below reads it, never Peripherals... itself.
    using Entries   = Detail::Entries<Peripherals...>;
    using OnEntries = Detail::OnEntries<Entries>;

    // The entries plus one generated dispatcher per shared vector (SharedIsr.hpp).
    using PeripheralList = MPL::Expand<Detail::WithSharedDispatchers, Entries>;

    // The list's checks (Checks.hpp): the same block as Startup's for its own list, so an error
    // here names its offender and core 1 too. The rules over both lists run in Startup.
    // The checks' view of the chip (StartUp.hpp's), its interrupt traits looked up late on this core's list.
    using Context = Detail::StartupContext<SecondaryCore>;

    static_assert(Checks::CheckList<Context,
                                    Checks::Role::secondary,
                                    1,
                                    SecondaryCore,
                                    Entries,
                                    PeripheralList>::value);

    // The extra argument makes the lookup dependent on Main: a chip package without the
    // specialisation (single-core parts) then compiles unless a SecondaryCore is actually
    // used, instead of failing in every build that includes StartUp.hpp.
    using Init = SecondaryCoreInit<Kvasir::Tag::User, std::integral_constant<void (*)(), Main>>;
    static_assert(Port::ChipSecondaryCore<Init>,
                  "Kvasir port: the chip package provides no Startup::SecondaryCoreInit<Tag::User> "
                  "with cpacrEnable "
                  "(chip/StartUp.hpp) - this chip has no second core in Kvasir");

    // ARM "udf #255": a runaway into unused stack faults, and the watermark is unambiguous.
    static constexpr std::uint32_t stackFill = 0xFFDEFFDE;

    // Declared before the vector table, whose type takes &entry: a static data member's
    // declaration is not a complete-class context, so the name has to be visible already.
    //
    // entry is the bootrom's target: asm only. It enables the coprocessors the chip layer
    // asks for (the FPU is off when the bootrom hands over, and a hard-float build touches
    // it in its first frame), arms MSPLIM and branches to start(). On a core with neither
    // (a Cortex-M0+) it is the branch alone: the bootrom has set SP and VTOR already.
    [[gnu::naked]] static void entry();

    [[noreturn]] static void start();

    // This core's ISRs, through the list's IsrTransform if an entry has one (Entry.hpp; IsrProfiling on core 1).
    using IsrList = typename Detail::
      IsrTransformInT<typename Detail::IsrsOf<PeripheralList>::type, 1, Entries>::type;
    using Table = Detail::VectorTable<std::addressof(_LINKER_stack1_end_),
                                      std::addressof(entry),
                                      typename Context::Traits,
                                      IsrList>;

    // gnu::used does not instantiate a class-template member; launch() taking the address
    // for VTOR is what keeps this in the image, and the linker script keeps the section.
    [[gnu::used,
      gnu::section(".core1_vectors"),
      gnu::aligned(Detail::vectorTableAlignment(Table::entries))]]
    static constexpr Table nvicIsrVectors{};

    // From core 0. Handshakes with the core the bootrom parked; if that fails (core 1 is
    // already running something after a debugger restart of core 0 alone) resets it and
    // tries once more. Then runs every peripheral's primarySync() against core 1's
    // secondarySync() (see the rendezvous hooks below), then waits, bounded, for start()
    // to report that core 1's Startup list has completed. False means core 1 is not
    // running our code, or a rendezvous timed out; the caller decides whether that is fatal.
    //
    // The stack is filled before each handshake attempt: the handshake ends with core 1
    // already executing entry(), so it cannot be filled afterwards. On the retry path the
    // fill follows reset(), so a core 1 that was running old code is stopped first.
    [[nodiscard]] static bool launch() {
        ready.store(false, std::memory_order_relaxed);

        auto const pc = reinterpret_cast<std::uint32_t>(std::addressof(entry));
        auto const sp = reinterpret_cast<std::uint32_t>(std::addressof(_LINKER_stack1_end_));
        auto const vt = reinterpret_cast<std::uint32_t>(std::addressof(nvicIsrVectors));

        OnEntries::template callEach<Detail::RuntimeHook::PrimaryPrepare>();
        bool launched = handshake(pc, sp, vt);
        if(!launched) {
            reset();
            launched = handshake(pc, sp, vt);
        }
        if(!launched) { return false; }

        if(!OnEntries::template allTrue<Detail::RuntimeHook::PrimarySync>()) { return false; }

        return waitReady();
    }

    // Hard reset through the chip's power-on state machine; core 1 returns to the bootrom's
    // holding pen and launch() can be called again. What core 1 was holding goes with it:
    // the SDK releases its own atomic-shim lock here, and every peripheral in the list gets
    // its primaryPrepare(); anything else the application shared with core 1 (a Spinlock
    // core 1 held, a Seqlock it was writing, a DMA channel it started) is the
    // application's to reset, from a primaryPrepare() of its own.
    static void reset() {
        Init::reset();
        ready.store(false, std::memory_order_relaxed);
        Kvasir::Atomic::onOtherCoreStopped();
        OnEntries::template callEach<Detail::RuntimeHook::PrimaryPrepare>();
    }

    // Whether every secondarySync() met its counterpart on the last launch.
    [[nodiscard]] static bool synchronised() { return syncedOk.load(std::memory_order_acquire); }

    // True once start() has run core 1's Startup list and handed over to Main.
    [[nodiscard]] static bool running() { return ready.load(std::memory_order_acquire); }

    // Every function this core's peripherals added to Hook (Port/Extends.hpp), in list order: call it
    // from Main for Hook::MainLoop on this core.
    template<typename Hook,
             typename... Args>
    [[gnu::always_inline]] static void run(Args const&... args) {
        Kvasir::Startup::Detail::dispatch<Hook, void>(static_cast<PeripheralList*>(nullptr),
                                                      args...);
    }

    // run<Hook>() plus when the loop is next needed (Port/Extends.hpp Kvasir::Turn), for a loop that sleeps.
    template<typename Hook,
             typename... Args>
    [[gnu::always_inline]] static Kvasir::Turn runTurn(Args const&... args) {
        return Kvasir::Startup::Detail::dispatch<Hook, Kvasir::Turn>(
          static_cast<PeripheralList*>(nullptr),
          args...);
    }

    // This core's list (its entries), for Executor::adopt: the generated dispatchers add no Extend.
    using LocalPeripherals = Entries;

    // Bytes of .stack1 that have ever been written since launch(), from the fill pattern.
    [[nodiscard]] static std::size_t stackHighWater() {
        auto const* const words = stackBegin();
        std::size_t const n     = stackSize() / sizeof(std::uint32_t);
        std::size_t       fill  = 0;
        while(fill < n && words[fill] == stackFill) { ++fill; }
        return (n - fill) * sizeof(std::uint32_t);
    }

    // On the symbols' addresses: two linker symbols are two objects to the compiler, their pointer difference UB.
    [[nodiscard]] static std::size_t stackSize() {
        return linkerAddress<_LINKER_stack1_end_>() - linkerAddress<_LINKER_stack1_start_>();
    }

    // Core 0's vectors the chip half needs while core 1 runs (RP: the NMI that parks core 0 after a fatal path on core
    // 1, Port::StopOtherCore); this entry is in core 0's list, so they go into core 0's table. armOtherCoreStop(),
    // before the launch, arms what they answer.
    using Isr = typename Detail::PrimaryIsrOf<Init>::type;

    // What makes a SecondaryCore a Startup-list element: core 0 launches it here. A core
    // that does not come up is fatal for a list-launched core: main() would run against a
    // dead partner (a firmware that wants to retry calls launch() from main instead).
    static void runtimeInit() {
        if constexpr(!Detail::anyNoCoreResetGuard<Entries>
                     && requires { Init::armCoreResetGuard(); })
        {
            Init::armCoreResetGuard();
        }
        if constexpr(requires { Init::armOtherCoreStop(); }) { Init::armOtherCoreStop(); }
        if(!launch()) {
            KVASIR_DETAIL_FATAL(
              UC_LOG_C("secondary core did not come up (handshake, rendezvous or ready wait)"),
              ::Kvasir::Panic::raise(::Kvasir::Panic::Cause::secondaryCore));
        }
    }

private:
    // Polls of the ready flag when no LaunchTimeout is listed. At 200 MHz roughly 0.2 s.
    static constexpr std::uint32_t readyBudget = 10'000'000;

    using Timeout = MPL::Expand<Detail::FirstLaunchTimeout, Entries>;

    static bool handshake(std::uint32_t pc,
                          std::uint32_t sp,
                          std::uint32_t vt) {
        fillStack();
        return Init::launch(pc, sp, vt);
    }

    static bool waitReady() {
        if constexpr(std::is_void_v<Timeout>) {
            for(std::uint32_t i = 0; i < readyBudget; ++i) {
                if(ready.load(std::memory_order_acquire)) { return true; }
            }
            return false;
        } else {
            auto const deadline = Timeout::clock::now() + Timeout::timeout;
            while(!ready.load(std::memory_order_acquire)) {
                if(Timeout::clock::now() > deadline) { return false; }
            }
            return true;
        }
    }

    static inline std::atomic<bool> ready{false};
    static inline std::atomic<bool> syncedOk{true};

    // Rendezvous hooks. A peripheral in this list may define
    //   static void primaryPrepare();   // core 0, before the bootrom handshake: reset state
    //   static bool primarySync();      // core 0, after the handshake, before the ready wait
    //   static bool secondarySync();    // core 1, after its runtime inits, before ready
    // The two sync sides run concurrently on their own cores and may spin on each other;
    // each must bound its own wait, because the ready wait only starts after primarySync()
    // returns. primaryPrepare() runs before every handshake and after every reset(): on
    // the retry path and on a relaunch after reset() core 1 is provably stopped, so
    // resetting shared state there is race-free. Only the very first attempt after a
    // core-0-only debugger restart can meet a core 1 still running the old image; that
    // handshake fails, reset() stops core 1, and the prepare runs again on clean ground.
    // They are called through Entry.hpp's table (Detail::callEach / Detail::allTrue).

    static std::uint32_t* stackBegin() {
        return reinterpret_cast<std::uint32_t*>(std::addressof(_LINKER_stack1_start_));
    }

    static std::uint32_t* stackEnd() {
        return reinterpret_cast<std::uint32_t*>(std::addressof(_LINKER_stack1_end_));
    }

    static void fillStack() { std::fill(stackBegin(), stackEnd(), stackFill); }
};

template<void (*Main)(),
         typename... Peripherals>
void SecondaryCore<Main,
                   Peripherals...>::entry() {
#if KVASIR_CORE_ARMV8M_MAIN
    // CPACR (0xE000ED88, DDI0553B.y D1.2.14, md l.69493) is read-modify-written so the bits
    // the bootrom set stay: cp7 on the RP2350, whose boot ROM runs RCP instructions on core 1
    // while it waits for the launch (RP2350 datasheet 3.6.3.1, md l.5085; a coprocessor needs
    // its CPACR bits, 3.6, md l.4562). The mask comes in a register: it is not a Thumb modified
    // immediate. dsb/isb make the enable take effect before the first instruction that could
    // use a coprocessor (an SCS write is visible after its completion and a Context
    // synchronization event, DDI0553B.y B7.2.16 RSJQJ/RNPDJ, md l.10750-10754).
    asm volatile(
      "movw r0, #0xED88   \n"   // CPACR
      "movt r0, #0xE000   \n"
      "ldr  r1, [r0]      \n"
      "orr  r1, r1, %0    \n"
      "str  r1, [r0]      \n"
      "dsb                \n"
      "isb                \n"
      "msr  msplim, %1    \n"
      "bx   %2            \n"
      :
      : "l"(Init::cpacrEnable),
        "l"(std::addressof(_LINKER_stack1_start_)),
        "l"(std::addressof(start))
      : "r0", "r1", "memory");
#else
    asm volatile("bx %0" : : "l"(std::addressof(start)) : "memory");
#endif
}

template<void (*Main)(),
         typename... Peripherals>
void SecondaryCore<Main,
                   Peripherals...>::start() {
    Init{}();

    OnEntries::template applyPhase<Detail::Phase::Early>();
    // the same sequence as core 0's, over the list with the shared-vector dispatchers (they add
    // only interrupt phases: the other phases and the hooks are those of Entries)
    MPL::Expand<Detail::InitPhases, PeripheralList>::run();

    syncedOk.store(OnEntries::template allTrue<Detail::RuntimeHook::SecondarySync>(),
                   std::memory_order_release);
    ready.store(true, std::memory_order_release);

    Main();

    assert(false);
    while(true) {}
}

}   // namespace Kvasir::Startup
