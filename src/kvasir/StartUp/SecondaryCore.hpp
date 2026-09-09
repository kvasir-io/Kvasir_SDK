#pragma once

#include "kvasir/Atomic/Atomic.hpp"
#include "kvasir/Common/Core.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/StartUp/LinkerSymbols.hpp"
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
inline constexpr bool MulticoreEnabled = true;
#else
inline constexpr bool MulticoreEnabled = false;
#endif

// How long launch() waits for the other core to report ready, on a clock the primary core
// can read. Listed in the SecondaryCore's list like a peripheral (it has no init steps):
//
//   using Core1 = SecondaryCore<&core1Main, LaunchTimeout<Clock, 2000>, ...>;
//
// Without one the wait is a fixed number of polls (ReadyBudget), which is a time only for
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
// 1's list has completed too, so when main() runs, both cores are configured. Startup
// static_asserts that no peripheral is in both lists and that nothing with a runtimeInit
// follows the SecondaryCore.
//
// The stack is the linker's .stack1, sized by CORE1_STACK_SIZE (kvasir_executable_variants),
// which also defines KVASIR_MULTICORE and with it the cross-core atomic shim and queue
// defaults. On Armv8-M mainline (the RP2350) MSPLIM is armed at entry, so an overflow is a
// StackOverflow fault on core 1 and nothing on core 0; a Cortex-M0+ (the RP2040) has no
// stack limit, and an overflow there runs into core 0's stack top unnoticed. The stack is
// filled with a pattern at launch, and stackHighWater() reads how much of it was ever used.
//
// The chip half (SecondaryCoreInit<Tag::User>: the CPACR mask, launch(), reset()) lives in
// the chip's StartUp.hpp, next to FirstInitStep.
template<void (*Main)(), typename... Peripherals>
struct SecondaryCore {
    static_assert(sizeof...(Peripherals) >= 0 && MulticoreEnabled,
                  "SecondaryCore needs a multicore build: pass CORE1_STACK_SIZE to "
                  "kvasir_executable_variants / target_configure_kvasir");

    using PeripheralList = brigand::list<Peripherals...>;

    // The list's shape (ListRules.hpp), the same rules Startup applies to its own list.
    static_assert(ListRules::NoDuplicateEntry<PeripheralList>::value,
                  "a peripheral is listed twice in one SecondaryCore list: its init steps "
                  "would run twice");
    static_assert(ListRules::AllArePeripherals<Detail::IsSecondaryCore,
                                               PeripheralList>::value,
                  "a type in the SecondaryCore list is not a peripheral (no init step, Isr, "
                  "runtime hook or Provides/Claims): a driver's Config listed instead of the "
                  "driver, or a typo'd alias - it would contribute nothing");
    static_assert(ListRules::NoSecondaryIn<Detail::IsSecondaryCore,
                                           PeripheralList>::value,
                  "a SecondaryCore inside a SecondaryCore's list: the chip has one other core, "
                  "and only the primary Startup list launches it");
    static_assert(ListRules::NoClockSettingsIn<PeripheralList>::value,
                  "a ClockSettings in a SecondaryCore list: the clock tree is chip-wide and "
                  "core 0's job (Startup's first argument)");
    static_assert(Detail::InterruptReports<SecondaryCore,
                                           PeripheralList>::value);
    static_assert(
      ListRules::EnabledLinesHandled<typename ListRules::EnabledInterruptsIn<PeripheralList>::type,
                                     typename Detail::IsrsOf<PeripheralList>::type>::value,
      "an init step enables an interrupt line for which nothing in this SecondaryCore's list "
      "installs an Isr (see the Report above for the index)");

    // The extra argument makes the lookup dependent on Main: a chip package without the
    // specialisation (single-core parts) then compiles unless a SecondaryCore is actually
    // used, instead of failing in every build that includes StartUp.hpp.
    using Init = SecondaryCoreInit<Kvasir::Tag::User, std::integral_constant<void (*)(), Main>>;

    // ARM "udf #255": a runaway into unused stack faults, and the watermark is unambiguous.
    static constexpr std::uint32_t StackFill = 0xFFDEFFDE;

    // Declared before the vector table, whose type takes &entry: a static data member's
    // declaration is not a complete-class context, so the name has to be visible already.
    //
    // entry is the bootrom's target: asm only. It enables the coprocessors the chip layer
    // asks for (the FPU is off when the bootrom hands over, and a hard-float build touches
    // it in its first frame), arms MSPLIM and branches to start(). On a core with neither
    // (a Cortex-M0+) it is the branch alone: the bootrom has set SP and VTOR already.
    [[gnu::naked]] static void entry();

    [[noreturn]] static void start();

    using IsrList = GetIsrPointersForT<std::addressof(_LINKER_stack1_end_),
                                       std::addressof(entry),
                                       Peripherals...>;

    // gnu::used does not instantiate a class-template member; launch() taking the address
    // for VTOR is what keeps this in the image, and the linker script keeps the section.
    [[gnu::used,
      gnu::section(".core1_vectors"),
      gnu::aligned(Detail::vectorTableAlignment(brigand::size<IsrList>::value))]]
    static constexpr NvicVectorTable<IsrList> nvicIsrVectors{};

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

        callPrimaryPrepares<Peripherals...>();
        bool launched = handshake(pc, sp, vt);
        if(!launched) {
            reset();
            launched = handshake(pc, sp, vt);
        }
        if(!launched) { return false; }

        if(!callPrimarySyncs<Peripherals...>()) { return false; }

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
        Kvasir::Atomic::onSecondaryCoreReset();
        callPrimaryPrepares<Peripherals...>();
    }

    // Whether every secondarySync() met its counterpart on the last launch.
    [[nodiscard]] static bool synchronised() { return syncedOk.load(std::memory_order_acquire); }

    // True once start() has run core 1's Startup list and handed over to Main.
    [[nodiscard]] static bool running() { return ready.load(std::memory_order_acquire); }

    // Bytes of .stack1 that have ever been written since launch(), from the fill pattern.
    [[nodiscard]] static std::size_t stackHighWater() {
        auto const* p   = stackBegin();
        auto const* end = stackEnd();
        while(p < end && *p == StackFill) { ++p; }
        return static_cast<std::size_t>(end - p) * sizeof(std::uint32_t);
    }

    [[nodiscard]] static std::size_t stackSize() {
        return static_cast<std::size_t>(stackEnd() - stackBegin()) * sizeof(std::uint32_t);
    }

    // What makes a SecondaryCore a Startup-list element: core 0 launches it here. A core
    // that does not come up is fatal for a list-launched core: main() would run against a
    // dead partner. Logged before the assert so a build without asserts still says so.
    static void runtimeInit() {
        [[maybe_unused]] bool const launched = launch();
        if(!launched) {
            UC_LOG_C("secondary core did not come up (handshake, rendezvous or ready wait)");
        }
        assert(launched);
    }

private:
    // Polls of the ready flag when no LaunchTimeout is listed. At 200 MHz roughly 0.2 s.
    static constexpr std::uint32_t ReadyBudget = 10'000'000;

    template<typename T>
      struct IsLaunchTimeout : std::bool_constant < requires {
        T::isLaunchTimeout;
    }>{};

    template<typename... Ts>
    struct FindLaunchTimeout {
        using type = void;
    };

    template<typename T, typename... Ts>
    struct FindLaunchTimeout<T, Ts...> {
        using type = std::
          conditional_t<IsLaunchTimeout<T>::value, T, typename FindLaunchTimeout<Ts...>::type>;
    };

    using Timeout = typename FindLaunchTimeout<Peripherals...>::type;

    static bool handshake(std::uint32_t pc,
                          std::uint32_t sp,
                          std::uint32_t vt) {
        fillStack();
        return Init::launch(pc, sp, vt);
    }

    static bool waitReady() {
        if constexpr(std::is_void_v<Timeout>) {
            for(std::uint32_t i = 0; i < ReadyBudget; ++i) {
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
    template<typename T>
    static void callPrimaryPrepare() {
        if constexpr(requires { T::primaryPrepare(); }) { T::primaryPrepare(); }
    }

    template<typename... Ts>
    static void callPrimaryPrepares() {
        (callPrimaryPrepare<Ts>(), ...);
    }

    template<typename T>
    static bool callPrimarySync() {
        if constexpr(requires {
                         { T::primarySync() } -> std::same_as<bool>;
                     })
        {
            return T::primarySync();
        } else {
            return true;
        }
    }

    template<typename... Ts>
    static bool callPrimarySyncs() {
        return (callPrimarySync<Ts>() && ...);
    }

    template<typename T>
    static bool callSecondarySync() {
        if constexpr(requires {
                         { T::secondarySync() } -> std::same_as<bool>;
                     })
        {
            return T::secondarySync();
        } else {
            return true;
        }
    }

    template<typename... Ts>
    static bool callSecondarySyncs() {
        return (callSecondarySync<Ts>() && ...);
    }

    static std::uint32_t* stackBegin() {
        return reinterpret_cast<std::uint32_t*>(std::addressof(_LINKER_stack1_start_));
    }

    static std::uint32_t* stackEnd() {
        return reinterpret_cast<std::uint32_t*>(std::addressof(_LINKER_stack1_end_));
    }

    static void fillStack() { std::fill(stackBegin(), stackEnd(), StackFill); }
};

template<void (*Main)(),
         typename... Peripherals>
void SecondaryCore<Main,
                   Peripherals...>::entry() {
#if defined(__ARM_ARCH_8M_MAIN__)
    // CPACR is read-modify-written so the bits the bootrom set (cp7 on the RP2350) stay.
    // The mask comes in a register: it is not a Thumb modified immediate. dsb/isb make the
    // enable take effect before the first instruction that could use a coprocessor.
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

    Kvasir::Register::apply(GetEarlyInitT<Peripherals...>{});
    Kvasir::Register::apply(GetPowerClockInitT<Peripherals...>{});
    Kvasir::Register::apply(GetPinInitT<Peripherals...>{});
    Kvasir::Register::apply(GetPeripheryInitT<Peripherals...>{});
    Kvasir::Register::apply(GetInterruptInitT<Peripherals...>{});
    callPreEnableRuntimeInits<Peripherals...>();
    Kvasir::Nvic::enable_all();
    Kvasir::Register::apply(GetPeripheryEnableInitT<Peripherals...>{});
    callRuntimeInits<Peripherals...>();

    syncedOk.store(callSecondarySyncs<Peripherals...>(), std::memory_order_release);
    ready.store(true, std::memory_order_release);

    Main();

    assert(false);
    while(true) {}
}

}   // namespace Kvasir::Startup
