#pragma once

// The core functions this file calls (Core::cleanCore, Nvic::clearAllEnablesAndPending) before the port's fallback
// declarations and checks (kvasir/Port/Core.hpp, Check.hpp): a non-dependent call binds where it is written.
#ifdef __arm__
    #include "core/Nvic.hpp"
    #include "core/StartUp.hpp"
#endif
#include "core/Special.hpp"
#include "kvasir/Common/Interrupt.hpp"
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/List.hpp"
#include "kvasir/Port/Capabilities.hpp"
#include "kvasir/Port/Check.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/Interrupts.hpp"
#include "kvasir/Port/LinkerSymbols.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/Runtime/CrcImageEngine.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "kvasir/Runtime/ubsan.hpp"
#include "kvasir/StartUp/Checks.hpp"
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/StartUp/IsrProfiler.hpp"
#include "kvasir/StartUp/ListRules.hpp"
#include "kvasir/StartUp/MemoryInit.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include "kvasir/StartUp/SafeState.hpp"
#include "kvasir/StartUp/SharedIsr.hpp"
#include "kvasir/StartUp/VectorTable.hpp"
#include "kvasir/Util/ImageDescriptor.hpp"
#include "kvasir/Util/attributes.hpp"
#include "uc_log/uc_log.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>   // std::terminate, defined by Runtime/Cxx.hpp
#include <new>
#include <string_view>
#include <type_traits>

// declaring and calling main is ill-formed in ISO C++ but intended here; gcc diagnoses it under -Wpedantic
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
extern "C" {
extern int main();
}
#pragma GCC diagnostic pop

#ifdef __arm__
extern "C" {
// The value every -fstack-protector function copies onto its stack on entry and compares on exit.
// In .noInit: neither initMemory() nor a reset touches it, so a protected function that is on the
// stack while .data/.bss are set up (the clock init) still finds the value it started with. Seeded
// once per boot by core 0 (Startup::Detail::seedStackGuard); volatile for the reason
// Fault::lastFault is (LTO narrows an uninitialised object otherwise). A wild write that hits it
// shows up as canary failures in unrelated functions: peek it and compare with its boot value.
[[gnu::used, KVASIR_NOINIT]] inline std::uint32_t volatile __stack_chk_guard;
}
#endif

namespace Kvasir { namespace Startup {
    namespace Detail {
        namespace br = brigand;

        // A phase member must be a Register::Action or a brigand::list (of actions and lists):
        // checked where the actions are applied; Entry.hpp's tables accept anything.
        template<typename T>
        inline constexpr bool isActionOrList = false;

        template<typename T, typename U>
        inline constexpr bool isActionOrList<Register::Action<T, U>> = true;

        template<typename... Ts>
        inline constexpr bool isActionOrList<br::list<Ts...>> = true;

        template<typename P, typename T>
        inline constexpr bool phaseMemberPlausible = [] {
            if constexpr(P::template has<T>) {
                return isActionOrList<typename P::template Raw<T>>;
            } else {
                return true;
            }
        }();

        // One phase of a list: every entry's actions, in list order (Entry.hpp PhaseActions).
        template<typename P,
                 typename... Ts>
        [[gnu::always_inline]] inline void applyPhase() {
            static_assert((phaseMemberPlausible<P, Ts> && ...),
                          "implausible type in a Startup phase member: expected a Register::Action "
                          "or a brigand::list");
            if constexpr((phaseMemberPlausible<P, Ts> && ...)) {   // else only the assert above
                Kvasir::Register::apply(PhaseActions<P, Ts...>{});
            }
        }

        // ExtractIsr / IsrsOf / isPerCoreVector: Checks.hpp (the rules read them too).
    }   // namespace Detail

    // Defined in SecondaryCore.hpp (which a main that starts a second core includes instead of this file). Declared
    // here so the guards in Startup can recognise one in a peripheral list.
    template<void (*Main)(), typename... Peripherals>
    struct SecondaryCore;

    namespace Detail {
        template<typename T>
        struct IsSecondaryCore : std::false_type {};

        template<void (*Main)(), typename... Ps>
        struct IsSecondaryCore<SecondaryCore<Main, Ps...>> : std::true_type {};

        template<typename T>
        struct SecondaryPeripherals {
            using type = brigand::list<>;
        };

        template<void (*Main)(), typename... Ps>
        struct SecondaryPeripherals<SecondaryCore<Main, Ps...>> {
            using type = Entries<Ps...>;   // its groups replaced by their members (Entry.hpp)
        };

        // A list's entries and every SecondaryCore's entries after them (what ExtendsOn reads).
        template<typename List>
        struct WithSecondaryEntries;

        template<typename... Ts>
        struct WithSecondaryEntries<brigand::list<Ts...>> {
            using type
              = brigand::flatten<brigand::list<Ts..., typename SecondaryPeripherals<Ts>::type...>>;
        };

        // The checks' view of this chip and runtime (Checks.hpp): the vector table, what a
        // SecondaryCore is, the log backend whose ISR contract counts even unlisted, and the image's
        // CRC engine hook (its set must be listed when it claims hardware; read when a list is checked).
        // Dep: the Startup's own parameter, so the chip's interrupt traits are a late lookup (Port::LateT).
        template<typename Dep>
        using StartupContext
          = Checks::Context<Port::LateT<Nvic::InterruptOffsetTraits<Kvasir::Tag::User>, Dep>,
                            IsSecondaryCore,
                            SecondaryPeripherals,
                            ::uc_log::ComBackend<::uc_log::Tag::User>,
                            ::Kvasir::Crc::ImageEngines<::Kvasir::Tag::User>>;
    }   // namespace Detail

    // .data from its load image, .bss zeroed (linker/common.ld's symbols; MemoryInit.hpp hides
    // them from the optimiser). A chip's extra RAM regions follow in ExtraMemoryInit, with the
    // same two helpers.
    //
    // A RAM-only image's .data has no AT() (common.ld): its load image is where it runs, so the
    // copy is onto itself - the same bytes. Not skipped: the test would cost every flash image a
    // compare and a branch, for a copy that changes nothing.
    [[gnu::always_inline]] inline void initMemory() {
        copyImage(std::addressof(_LINKER_data_start_flash_),
                  std::addressof(_LINKER_data_start_),
                  reinterpret_cast<std::size_t>(std::addressof(_LINKER_data_size_)));

        zeroRange(std::addressof(_LINKER_bss_start_),
                  reinterpret_cast<std::size_t>(std::addressof(_LINKER_bss_size_)));
    }

    [[gnu::always_inline]] inline void callGlobalConstructors() {
        auto initBegin = std::addressof(_LINKER_init_array_start_);
        asm("" : "+l"(initBegin)::);

        auto initEnd = std::addressof(_LINKER_init_array_end_);
        asm("" : "+l"(initEnd)::);

        while(initBegin < initEnd) {
            (*initBegin)();
            ++initBegin;
        }
    }

    namespace Detail {

        // Once per boot on core 0, while no protected function that will return is on the stack
        // (ResetISR never returns and this inlines into it). Core 1 shares the guard and never
        // seeds. The low byte is zero: a terminator, so an unbounded string copy cannot write the
        // canary back.
        // Dep makes the chip's StackGuardEntropy a late lookup (Port::LateT): named non-dependently, the defined
        // default would be taken where this is parsed, and the chip's later partial specialisation ignored.
        template<typename Dep>
        [[gnu::always_inline,
          gnu::no_stack_protector]] inline void
        seedStackGuard() {
#ifdef __arm__
            __stack_chk_guard
              = Port::LateT<StackGuardEntropy<Kvasir::Tag::User>, Dep>{}() & 0xFFFFFF00U;
#endif
        }

        // An image does not always start from a clean reset: a RAM image the debugger starts, or an
        // application a bootloader jumps into, inherits whatever ran before (e.g. the RP2040 boot ROM's
        // USB interrupt, enabled and pending). Every NVIC enable and pending bit is cleared before
        // anything else; Startup enables what the list claims. The words and their registers: the core
        // package (core_cortex_common Nvic.hpp). Dep makes the chip's interrupt traits a late lookup (Port::LateT), so
        // this header may come before the chip's.
        template<typename Dep>
        inline void disableAllInterrupts() {
#ifdef __arm__
            Nvic::clearAllEnablesAndPending<
              Port::LateT<Nvic::InterruptOffsetTraits<Kvasir::Tag::User>, Dep>>();
#endif
        }

        // What a warm reset would have done and the entry stub (KVASIR_RESET_ENTRY_ASM) does not. An image not
        // entered from a reset (a RAM image the debugger starts, an application a bootloader jumps into, an RP
        // debugger reset: SYSRESETREQ restarts the core only, RP2040 DS 2.4.2.9) inherits a running SysTick, a
        // pending PendSV/SysTick, FAULTMASK, the FPU's lazy state and old fault status bits; SysTick and PendSV
        // are stopped first. What a core has of these, and the registers: the core package's Core::cleanCore()
        // (core/StartUp.hpp).
        [[gnu::always_inline]] inline void cleanCore() {
#ifdef __arm__
            Core::cleanCore();
#endif
        }

        // The whole body of the naked ResetISR (Startup::ResetISR). Naked, so no compiled code runs
        // first: a bootloader or debugger may enter with PRIMASK clear, CONTROL.SPSEL = 1 or a stale MSPLIM above
        // SP, and a prologue's `sub sp` would itself be the STKOF UsageFault (DDI0553B.y B3.21 IBJHX; MSR to
        // MSPLIM/PSPLIM is not limit-checked, RBRLT). PRIMASK = 1 until Core::unmaskIrqs(); MSPLIM/PSPLIM = 0 as
        // after a warm reset (D1.2.177; Core::startup() arms MSPLIM again); CONTROL = 0: privileged, MSP, FPCA = 0
        // (DDI0419E B1.4.4, DDI0403E B1.4.4, DDI0553B.y B3.4), then ISB (DDI0553B.y B3.4, DDI0403E B5.2.3); MSP =
        // vector table word 0. Then the noreturn C++ body (the literal pool may follow the bl). `.syntax unified`:
        // gcc passes Thumb-1 inline asm in divided syntax (FaultHandler.hpp). Also names the entry ResetISR for
        // `--entry=ResetISR` (compiler_common.cmake), debuggers and backtraces; two Startups in one image are a
        // duplicate symbol at link time. Operands: 0 the body, 1 the entry itself, 2 _LINKER_stack_end_.
#if KVASIR_CORE_HAS_STACK_LIMITS
    #define KVASIR_RESET_ENTRY_LIMITS \
        "msr msplim, r0 \n"           \
        "msr psplim, r0 \n"
#else
    #define KVASIR_RESET_ENTRY_LIMITS ""
#endif
#define KVASIR_RESET_ENTRY_ASM                                             \
    ".globl ResetISR                         \n"                           \
    ".thumb_set ResetISR, %c1                \n"                           \
    ".syntax unified                         \n"                           \
    "cpsid i                                 \n"                           \
    "movs r0, #0                             \n" KVASIR_RESET_ENTRY_LIMITS \
    "msr control, r0                         \n"                           \
    "isb                                     \n"                           \
    "ldr r0, =%c2                            \n"                           \
    "msr msp, r0                             \n"                           \
    "bl %c0                                  \n"                           \
    ".ltorg                                  \n"

        // Power clock .. runtime inits, in the order ListRules::IsrPrioritiesIn assumes (every
        // entry's interrupt config, then every entry's periphery enable). The same on both cores:
        // core 0 runs the clocks and the memory init before it, core 1 its chip init and earlyInit.
        template<typename... Ps>
        struct InitPhases {
            [[gnu::always_inline]] static void run() {
                applyPhase<Phase::PowerClock, Ps...>();
                applyPhase<Phase::Pins, Ps...>();
                applyPhase<Phase::Periphery, Ps...>();
                applyPhase<Phase::Interrupts, Ps...>();
                callEach<RuntimeHook::PreEnableRuntimeInit, Ps...>();
                Kvasir::Core::unmaskIrqs();
                applyPhase<Phase::Enable, Ps...>();
                callEach<RuntimeHook::RuntimeInit, Ps...>();
            }
        };

        // The C++ start-up of core 0 (Startup::resetBody): clocks, memory, constructors, then the list's phases.
        template<typename ClockSettings, typename List>
        struct StartupImpl;

        template<typename ClockSettings, typename... Peripherals>
        struct StartupImpl<ClockSettings, brigand::list<Peripherals...>> {
            [[noreturn,
              gnu::always_inline]] static void
            ResetISR() {
                Detail::cleanCore();
                Port::LateT<FirstInitStep<Kvasir::Tag::User>, ClockSettings>{}();
                Detail::disableAllInterrupts<ClockSettings>();

                applyPhase<Phase::Early, Peripherals...>();

                ClockSettings::coreClockInit();

                callEach<RuntimeHook::PreMemoryInit, Peripherals...>();
                initMemory();
                Port::LateT<ExtraMemoryInit<Kvasir::Tag::User>,
                            ClockSettings>{}();   // the chip's extra RAM, if any
                callEach<RuntimeHook::PostMemoryInit, Peripherals...>();

                // after the clocks (a chip's entropy may need them), before any constructor
                Detail::seedStackGuard<ClockSettings>();

                callGlobalConstructors();

                ClockSettings::peripheryClockInit();

                InitPhases<Peripherals...>::run();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
                main();
#pragma GCC diagnostic pop
                assert(false);
            }
        };

    }   // namespace Detail

    template<typename ClockSettings, typename... Peripherals>
    struct Startup {
        // The chip's own entries (Startup::ChipEntries, Entry.hpp; empty on most chips), named late on the ClockSettings
        using ChipEntrySet =
          typename Port::LateT<ChipEntries<Kvasir::Tag::User>, ClockSettings>::type;
        using ChipEntryList
          = Detail::Entries<Detail::ChipEntriesGroup<ClockSettings, ChipEntrySet>>;

        // The list as Startup runs it: the chip's own entries first, then every listed entry with each group entry
        // (Entry.hpp, Runtime::Standard) replaced by its members in its place; the list itself when the chip adds none
        // and it has no group.
        using Entries = brigand::append<ChipEntryList, Detail::Entries<Peripherals...>>;

        // the chip adds its entries itself: listed by hand they would run twice (ListRules ChipEntryNotListed)
        static_assert(
          Checks::reported<
            Checks::ChipEntryListed,
            Checks::Detail::OnCore<0,
                                   ListRules::ListedChipEntries<Detail::Entries<Peripherals...>>>>);

        // The entries plus one generated dispatcher per vector their SubIsrs share
        // (SharedIsr.hpp); the entries themselves when none declares a SubIsr.
        using AllPeripherals = MPL::Expand<Detail::WithSharedDispatchers, Entries>;

        // The checks (Checks.hpp): this core's list, then the rules over the whole image. Every
        // rule is its offender list; each offender is a report that names it, core 1's too.
        static_assert(Checks::CheckList<Detail::StartupContext<ClockSettings>,
                                        Checks::Role::primary,
                                        0,
                                        Startup,
                                        Entries,
                                        AllPeripherals>::value);
        static_assert(Checks::ImageChecks<Detail::StartupContext<ClockSettings>,
                                          Startup,
                                          ClockSettings,
                                          Entries,
                                          AllPeripherals>::value);

        // The reset entry (vector table word 1): KVASIR_RESET_ENTRY_ASM, then resetBody. Neither carries profile
        // counters (KVASIR_NO_PROFILE): an increment would come before `cpsid i` and before initMemory().
        [[gnu::naked,
          gnu::noinline,
          KVASIR_NO_PROFILE]] static void
        ResetISR() {
#ifdef __arm__
            asm volatile(KVASIR_RESET_ENTRY_ASM ::"i"(&resetBody),
                         "i"(&ResetISR),
                         "i"(&_LINKER_stack_end_));
#endif
        }

        // the C++ body behind the naked reset entry (KVASIR_RESET_ENTRY_ASM): not naked itself, gcc builds no frame
        // for a naked function and the body is the whole start-up in C++
        [[noreturn,
          gnu::noinline,
          KVASIR_NO_PROFILE]] static void
        resetBody() {
            Detail::StartupImpl<ClockSettings, AllPeripherals>::ResetISR();
        }

        // The ISRs of the vector table: every entry's (and dispatcher's), through the list's IsrTransform if an entry
        // has one (Entry.hpp; IsrProfiling). The checks above read the list untransformed.
        using IsrList = typename Detail::
          IsrTransformInT<typename Detail::IsrsOf<AllPeripherals>::type, 0, Entries>::type;

        [[gnu::used, gnu::section(".core_vectors")]] static constexpr Detail::VectorTable<
          std::addressof(_LINKER_stack_end_),
          &ResetISR,
          typename Detail::StartupContext<ClockSettings>::Traits,
          IsrList> nvicIsrVectors{};

        // Every Extend the listed peripherals of both cores (a SecondaryCore's list included) added to Hook, in list
        // order, as a brigand::list: what a component that needs the list itself reads (Health::Supervisor).
        template<typename Hook>
        using ExtendsOn = MPL::Filter<
          Detail::OfHook<Hook>,
          MPL::Expand<Detail::AllExtends, typename Detail::WithSecondaryEntries<Entries>::type>>;

        // Every function the listed peripherals added to Hook (their `Extends`, Port/Extends.hpp), in
        // list order. A hook nobody extends is an empty call.
        template<typename Hook,
                 typename... Args>
        [[gnu::always_inline]] static void run(Args const&... args) {
            Detail::dispatch<Hook, void>(static_cast<Entries*>(nullptr), args...);
        }

        // The same calls as run<Hook>(), and when the loop is next needed: the earliest of what the functions
        // returned (Port/Extends.hpp Kvasir::Turn). For a loop that sleeps (Runtime/Executor.hpp).
        template<typename Hook,
                 typename... Args>
        [[gnu::always_inline]] static Kvasir::Turn runTurn(Args const&... args) {
            return Detail::dispatch<Hook, Kvasir::Turn>(static_cast<Entries*>(nullptr), args...);
        }

        // The chip's own entries' functions on Hook (ChipEntryList above): what a loop that calls its entries' turns by
        // hand instead of run<Hook>() adds once a turn (RP: Clocks::DegradedBoot's switch-up). An empty call without chip
        // entries, so it is the same line on every chip.
        template<typename Hook,
                 typename... Args>
        [[gnu::always_inline]] static void runChipEntries(Args const&... args) {
            Detail::dispatch<Hook, void>(static_cast<ChipEntryList*>(nullptr), args...);
        }

        // This core's list (its entries), for Executor::adopt and its boot report.
        using LocalPeripherals = Entries;
    };

}}   // namespace Kvasir::Startup

namespace Kvasir::Startup {
// The one line of a firmware that names its Startup type, after the type:
//     template struct Kvasir::Startup::Start<Startup>;
// The explicit instantiation instantiates `vectors`, which odr-uses the vector table (a
// static member of a class template exists only once something uses it) and through it
// the reset entry. Forgetting it leaves the image without a vector table, which the link
// refuses (common.ld asserts that ResetISR is defined). A list that extends Hook::SafeState
// names its safe state as the fatal start before this line (StartUp/SafeState.hpp): after it,
// the specialisation would come after the check instantiated the hook.
template<typename S>
struct Start {
    // a list with Hook::SafeState entries runs them on every fatal path (StartUp/SafeState.hpp)
    static_assert(Detail::safeStateWired<S,
                                         typename Port::LateT<Panic::FatalStartReaction<Tag::User>,
                                                              S>::type>());
#ifdef __arm__
    // the chip and core packages provide what this Startup needs (Port/Check.hpp); first, so its messages come first
    static_assert(Port::checkCore0<S,
                                   decltype(Core::cleanCore()),
                                   decltype(Nvic::clearAllEnablesAndPending())>());
    static constexpr auto const* vectors = std::addressof(S::nvicIsrVectors);
#endif
};
}   // namespace Kvasir::Startup

// The image's libc / libc++ / AEABI stand-ins and the panic entry points: strong definitions that exist once, in the
// TU with main (this header's). Same place in the TU, and the same order of definitions, as before the split.
//
// One translation unit per image includes this header (or SecondaryCore.hpp, which includes it): the main TU, after
// or before its chip and board headers (any order: the chip's hooks are looked up late, Port/README.md). Every other
// header - chip/chip.hpp, chip/drivers.hpp, the SDK's - may be in any number of TUs. A second TU with this header
// fails the link on the symbol All.hpp defines for it (kvasir_StartUp_hpp_is_included_by_two_TUs_of_one_image).
#include "kvasir/StartUp/Runtime/All.hpp"
