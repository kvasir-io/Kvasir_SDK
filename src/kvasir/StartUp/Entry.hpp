#pragma once

// The Startup entry protocol: every member a type in a Startup or SecondaryCore list may declare, and what reads it.
// Startup, SecondaryCore, Resources.hpp and ListRules.hpp detect the init phases and the runtime hooks below ONLY
// through this header.
//
//   register init phases (a Register::Action, or a brigand::list of actions and lists), in run order:
//     earlyInit                before the clocks (per core: SCB state such as fault enables)
//     powerClockEnable         after the periphery clocks: resets released, block clocks on
//     initStepPinConfig        pin functions and pads
//     initStepPeripheryConfig  block configuration
//     initStepInterruptConfig  priorities, clear-pending          (every entry's, then ...)
//     initStepPeripheryEnable  enables, after Core::unmaskIrqs()   (... every entry's: ListRules IsrPrioritiesIn)
//   runtime hooks (static, no parameters; the return type is checked, RuntimeHook below):
//     preMemoryInit / postMemoryInit       void   core 0, around .data/.bss
//                                                 (pre: only plain code, no static is set yet - uc_log's RTT
//                                                 epoch moves before its control block is rewritten; post:
//                                                 before any constructor)
//     preEnableRuntimeInit                 void   before Core::unmaskIrqs()
//     runtimeInit                          void   after the enables, list order
//     primaryPrepare                       void   SecondaryCore lists: core 0, before every launch
//     primarySync / secondarySync          bool   SecondaryCore lists: the rendezvous
//   read elsewhere (named here so the page is complete):
//     Isr / isr / SubIsrs (StartUp.hpp, SharedIsr.hpp), Extends / DrivenByHandComponents (Port/Extends.hpp; the
//     verbs table is Port/Hooks.hpp's: turn() is the main-loop step), Provides / Claims / startupCore
//     (Resources.hpp), isLaunchTimeout / noCoreResetGuard (SecondaryCore.hpp), isStartupEntry,
//     isrPriorityLevels / singleIsrPriorityLevel / silentIsrPriorityLevels / perCoreIsrContexts (ListRules.hpp),
//     isBootReport / bootReportInMain / isBootGuard / isHealthService / isWatchdog (ListRules.hpp, the boot report rules)
//   the ISR list transform (IsrTransformOf below; at most one entry per list: Checks.hpp):
//     template<class Isrs, unsigned Core> using IsrTransform = ...;
//                                       the list's ISRs in, the ISRs core Core's vector table holds out (IsrProfiling
//                                       wraps the profiled vectors); every ISR keeps its IType, the checks see the
//                                       list untransformed
//     perCoreEntry (static constexpr bool true)   the same entry may be in core 0's list and a SecondaryCore's: it
//                                       holds nothing chip-wide, each core runs its own copy (IsrProfiling)
//
//     chipEntry (static constexpr bool true)      one of the chip's own entries (ChipEntries below): never listed by
//                                       hand (ListRules ChipEntryNotListed)
//
// The chip package's own entries: Startup::ChipEntries<Tag::User> (a hook, Port/Hooks.hpp's table), specialised by a
// chip with `using type = X;`, X with `template<typename ClockSettings> using For = brigand::list<...>;` (the shape of
// Crc::ImageEngines) - entries every primary Startup list of
// the image runs before its listed ones, given the image's ClockSettings (chip_rp_common DegradedBoot.hpp: the RP
// chips' Clocks::DegradedBoot). None by default; a SecondaryCore's list gets none.
//
// A group entry stands for several entries in its place, and has no other member:
//     struct G { using StartupEntries = brigand::list<A, B, C>; };
// Startup and SecondaryCore read their lists through Entries<...> below: every group replaced by its members (groups
// in groups too), so every rule, Isr, Provides/Claims, Extends and marker is a member's, in the group's place. Groups
// are read there once: every table here (and Port/Extends.hpp's Extends, Resources.hpp's Provides/Claims, Checks.hpp's
// ISRs) reads one entry, and a group read as one entry has none of them.
//
// Both cores run the same sequence of phases and hooks (StartUp.hpp Detail::InitPhases): core 0 adds the clocks
// and the memory init before it, core 1 its chip init and earlyInit.
//
// Host-compilable: no chip, no register layer. The generic tables accept a phase member of any type (the host
// tests use stand-in actions); StartUp.hpp checks for a Register::Action or a list where it applies them.

#include "kvasir/Mpl/brigand.hpp"

#include <string_view>
#include <type_traits>

namespace Kvasir { namespace Startup { namespace Detail {

    // A phase member is one action or a list of actions and lists: as a list.
    template<typename T>
    struct AsList {
        using type = brigand::list<T>;
    };

    template<typename... Ts>
    struct AsList<brigand::list<Ts...>> {
        using type = brigand::list<Ts...>;
    };

    // ---- group entries ----------------------------------------------------------------------------
    template<typename T>
    inline constexpr bool isStartupGroup = requires { typename T::StartupEntries; };

    template<typename List>
    struct EntriesInT;

    template<typename T>
    struct EntriesOfOne {
        using type = brigand::list<T>;
    };

    template<typename T>
        requires isStartupGroup<T>
    struct EntriesOfOne<T> {
        using type = typename EntriesInT<typename T::StartupEntries>::type;
    };

    // The entries of a list as Startup runs them: each group replaced by its members, in its place.
    template<typename... Ts>
    using Entries = brigand::append<brigand::list<>, typename EntriesOfOne<Ts>::type...>;

    template<typename... Ts>
    struct EntriesInT<brigand::list<Ts...>> {
        using type = Entries<Ts...>;
    };

    template<typename List>
    using EntriesIn = typename EntriesInT<List>::type;

    template<typename T>
    inline constexpr bool isChipEntry = requires { requires T::chipEntry; };

    // ---- the ISR list transform -------------------------------------------------------------------
    template<typename T>
    inline constexpr bool hasIsrTransform
      = requires { typename T::template IsrTransform<brigand::list<>, 0>; };

    template<typename T>
    inline constexpr bool isPerCoreEntry = requires { requires T::perCoreEntry; };

    // The ISRs core Core's vector table holds: Isrs through the first entry's IsrTransform; Isrs without one.
    template<typename Isrs, unsigned Core, typename... Ts>
    struct IsrTransformOf {
        using type = Isrs;
    };

    template<typename Isrs, unsigned Core, typename T, typename... Ts>
    struct IsrTransformOf<Isrs, Core, T, Ts...> : IsrTransformOf<Isrs, Core, Ts...> {};

    template<typename Isrs, unsigned Core, typename T, typename... Ts>
        requires hasIsrTransform<T>
    struct IsrTransformOf<Isrs, Core, T, Ts...> {
        using type = typename T::template IsrTransform<Isrs, Core>;
    };

    template<typename Isrs, unsigned Core, typename List>
    struct IsrTransformInT;

    template<typename Isrs, unsigned Core, typename... Ts>
    struct IsrTransformInT<Isrs, Core, brigand::list<Ts...>> : IsrTransformOf<Isrs, Core, Ts...> {};

    // ---- the init phases ------------------------------------------------------------------------
    namespace Phase {
#define KVASIR_DETAIL_STARTUP_PHASE(Name, member)             \
    struct Name {                                             \
        static constexpr std::string_view name = #member;     \
        template<typename T>                                  \
        static constexpr bool has = requires { T::member; };  \
        template<typename T>                                  \
        using Raw = std::remove_cvref_t<decltype(T::member)>; \
    };

        KVASIR_DETAIL_STARTUP_PHASE(Early,
                                    earlyInit)
        KVASIR_DETAIL_STARTUP_PHASE(PowerClock,
                                    powerClockEnable)
        KVASIR_DETAIL_STARTUP_PHASE(Pins,
                                    initStepPinConfig)
        KVASIR_DETAIL_STARTUP_PHASE(Periphery,
                                    initStepPeripheryConfig)
        KVASIR_DETAIL_STARTUP_PHASE(Interrupts,
                                    initStepInterruptConfig)
        KVASIR_DETAIL_STARTUP_PHASE(Enable,
                                    initStepPeripheryEnable)
#undef KVASIR_DETAIL_STARTUP_PHASE

        using All = brigand::list<Early, PowerClock, Pins, Periphery, Interrupts, Enable>;
    }   // namespace Phase

    // One entry's actions of one phase, flattened; empty without the member.
    template<typename T, typename P>
    struct PhaseActionsOf {
        using type = brigand::list<>;
    };

    template<typename T, typename P>
        requires P::template
    has<T> struct PhaseActionsOf<T, P> {
        using type = brigand::flatten<typename AsList<typename P::template Raw<T>>::type>;
    };

    // A whole list's actions of one phase: per entry in list order, flattened.
    template<typename P, typename... Ts>
    using PhaseActions = brigand::flatten<brigand::list<typename PhaseActionsOf<Ts, P>::type...>>;

    template<typename P, typename List>
    struct PhaseActionsInT;

    template<typename P, typename... Ts>
    struct PhaseActionsInT<P, brigand::list<Ts...>> {
        using type = PhaseActions<P, Ts...>;
    };

    template<typename T, typename Phases = Phase::All>
    inline constexpr bool hasPhaseMember = false;

    template<typename T, typename... Ps>
    inline constexpr bool hasPhaseMember<T, brigand::list<Ps...>> = (Ps::template has<T> || ...);

    // ---- the runtime hooks Startup and SecondaryCore call -----------------------------------
    // One row per hook: its member name and the signature it must have. Startup detects a hook
    // by exactly the call `T::name()`, so a member of that name that cannot be called so, or
    // returns something else, would never be called: ListRules::HookShapesHold makes that a
    // compile error. A static member function or a static callable object (a lambda), no
    // parameters.
#define KVASIR_DETAIL_STARTUP_HOOK(Name, member, Ret, otherwise)                                \
    struct Name {                                                                               \
        static constexpr std::string_view name      = #member;                                  \
        static constexpr std::string_view signature = "static " #Ret " " #member "()";          \
        template<typename T>                                                                    \
        static constexpr bool declared = requires { &T::member; } || requires { T::member(); }; \
        template<typename T>                                                                    \
        static constexpr bool callable = requires { T::member(); };                             \
        template<typename T>                                                                    \
        static consteval bool shapeOk() {                                                       \
            if constexpr(callable<T>) {                                                         \
                return std::is_same_v<decltype(T::member()), Ret>;                              \
            } else {                                                                            \
                return !declared<T>;                                                            \
            }                                                                                   \
        }                                                                                       \
        /* the call when T has the hook; without it `otherwise` (true for a bool hook) */       \
        template<typename T>                                                                    \
        static Ret callIf() {                                                                   \
            if constexpr(callable<T>) {                                                         \
                return T::member();                                                             \
            } else {                                                                            \
                otherwise                                                                       \
            }                                                                                   \
        }                                                                                       \
    };

    namespace RuntimeHook {
        KVASIR_DETAIL_STARTUP_HOOK(PreMemoryInit,
                                   preMemoryInit,
                                   void,
                                   )   // before .data/.bss are set
        KVASIR_DETAIL_STARTUP_HOOK(PostMemoryInit,
                                   postMemoryInit,
                                   void,
                                   )   // right after them
        KVASIR_DETAIL_STARTUP_HOOK(PreEnableRuntimeInit,
                                   preEnableRuntimeInit,
                                   void,
                                   )   // before the interrupt enables
        KVASIR_DETAIL_STARTUP_HOOK(RuntimeInit,
                                   runtimeInit,
                                   void,
                                   )   // after them, in list order
        KVASIR_DETAIL_STARTUP_HOOK(PrimaryPrepare,
                                   primaryPrepare,
                                   void,
                                   )   // SecondaryCore lists: before a launch
        KVASIR_DETAIL_STARTUP_HOOK(PrimarySync,
                                   primarySync,
                                   bool,
                                   return true;)   // SecondaryCore lists: core 0's rendezvous
        KVASIR_DETAIL_STARTUP_HOOK(SecondarySync,
                                   secondarySync,
                                   bool,
                                   return true;)   // SecondaryCore lists: core 1's rendezvous

        using All = brigand::list<PreMemoryInit,
                                  PostMemoryInit,
                                  PreEnableRuntimeInit,
                                  RuntimeInit,
                                  PrimaryPrepare,
                                  PrimarySync,
                                  SecondarySync>;

        template<typename T, typename Hooks = All>
        struct DeclaresAny;

        template<typename T, typename... Hs>
        struct DeclaresAny<T, brigand::list<Hs...>>
          : std::bool_constant<(Hs::template declared<T> || ...)> {};

        template<typename T, typename Hooks = All>
        struct ShapesOk;

        template<typename T, typename... Hs>
        struct ShapesOk<T, brigand::list<Hs...>>
          : std::bool_constant<(Hs::template shapeOk<T>() && ...)> {};
    }   // namespace RuntimeHook

#undef KVASIR_DETAIL_STARTUP_HOOK

    // A hook of every entry that has it, in list order. HookShapesHold (asserted in both check
    // blocks) makes every declared hook one that is callable with its signature, so "callable"
    // here is "declared".
    template<typename H,
             typename... Ts>
    void callEach() {
        (H::template callIf<Ts>(), ...);
    }

    // The bool hooks (the rendezvous): true without the hook; the first false stops the rest.
    template<typename H,
             typename... Ts>
    bool allTrue() {
        return (H::template callIf<Ts>() && ...);
    }
}}}   // namespace Kvasir::Startup::Detail

namespace Kvasir { namespace Startup {
    // The hook (the header above): the chip's entries for every primary list, the image's ClockSettings handed in.
    struct NoChipEntries {
        template<typename ClockSettings>
        using For = brigand::list<>;
    };

    template<typename T, typename... Ts>
    struct ChipEntries {
        using type = NoChipEntries;
    };

    namespace Detail {
        // The chip's entries as one group entry (an empty one without a specialisation: Entries<> is then the list as
        // written). `Set` named late (Port::LateT on the ClockSettings): the specialisation is the chip's, seen where the
        // image's Startup is instantiated.
        template<typename ClockSettings, typename Set>
        struct ChipEntriesGroup {
            using StartupEntries = typename Set::template For<ClockSettings>;
        };
    }   // namespace Detail
}}   // namespace Kvasir::Startup
