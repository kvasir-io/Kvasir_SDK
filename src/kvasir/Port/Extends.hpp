#pragma once
// Named hooks a Startup list's peripherals add functions to, run by whoever owns the hook:
//
//     struct Master {
//         static void turn();   // timeouts, completion polling
//         using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Master::turn>;
//     };
//
//     while(true) {
//         Startup::run<Kvasir::Hook::MainLoop>();   // every peripheral's turn, in list order
//         application();
//     }
//
// Several at once: `using Extends = brigand::list<Extend<A, &f>, Extend<B, &g>>;`. A project
// defines its own hook the same way the SDK's are defined, a type with a Signature, and passes
// the arguments to run<Hook>(args...), by const reference to every function. Return values are
// dropped. Order is the Startup list's, as for the init phases. A fold of direct calls: the
// same code as the hand-written calls.
//
// A loop that sleeps (Runtime/Executor.hpp) runs Startup::runTurn<Hook>() instead: the same calls, and each function
// may say when it next needs the loop by what it returns - Kvasir::Turn (Turn::within(d), Turn::idle()), bool ("did
// work": busy if true, idle if false) or void (unknown, so busy: a function never converted keeps the loop awake).
// The fold keeps the earliest. run<Hook>() still drops the values.
//
// The verbs of a component, one meaning each:
//   turn()            the main-loop step: static, no parameters, never blocks; returns Kvasir::Turn (when the loop is
//                     next needed), bool ("did work") or void (unknown: busy). Listed with
//                     `using Extends = Extend<Hook::MainLoop, &turn>;`, or called by hand - not both. Every SDK, chip
//                     and driver entry on Hook::MainLoop is named so (Health::Service<> drives a supervisor's turn()).
//   isr() / Isr       the interrupt entry the Startup list wires
//   handler(onEvent)  event delivery to a callback (buttons, encoders): not a loop step
//
// A component the application calls by hand is declared so, in the list: `Kvasir::Startup::DrivenByHand<A, B>`. A
// firmware that runs run<Hook>() / runTurn<Hook>() then does not compile if A or B is ALSO a listed entry with an
// Extend on that Hook - the "runs twice a turn" trap, caught when a driver gains an Extends. Opt-in: a hand call that
// is not declared (a nested wait loop that turns a bus once more) stays legal.
// What this cannot enforce: that the application calls Startup::run<Hook::MainLoop>() at all, nor undeclared hand
// calls.

#include "kvasir/Common/Tags.hpp"
#include "kvasir/Mpl/List.hpp"
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/StartUp/Entry.hpp"
#include "kvasir/Util/Diagnostic.hpp"

#include <chrono>
#include <cstdint>
#include <type_traits>

namespace Kvasir {
// What a hook function may return instead of void: when it next needs the loop. Combined with |, the earliest wins.
class Turn {
public:
    using duration = std::chrono::microseconds;

    static constexpr Turn busy() { return Turn{duration::zero()}; }   // run the loop again at once

    static constexpr Turn idle() {
        return Turn{duration::max()};
    }   // only an interrupt or an event brings work

    template<typename Rep,
             typename Period>
    static constexpr Turn within(std::chrono::duration<Rep,
                                                       Period> d) {
        auto const us = std::chrono::ceil<duration>(d);
        if(us <= duration::zero()) { return busy(); }
        return Turn{us == duration::max() ? duration::max() - duration{1} : us};
    }

    constexpr Turn operator|(Turn o) const { return Turn{in_ < o.in_ ? in_ : o.in_}; }

    [[nodiscard]] constexpr bool isBusy() const { return in_ == duration::zero(); }

    [[nodiscard]] constexpr bool isIdle() const { return in_ == duration::max(); }

    /// How soon; duration::max() when idle.
    [[nodiscard]] constexpr duration in() const { return in_; }

    constexpr bool operator==(Turn const&) const = default;

private:
    constexpr explicit Turn(duration d) : in_{d} {}

    duration in_;
};
}   // namespace Kvasir

namespace Kvasir::Hook {
// Once per turn of the application's main loop. Must not block.
struct MainLoop {
    using Signature = void();
};

// Put the hardware into its safe state (outputs off, heater off), on the way to a reset or a
// halt: the image's fatal start (Startup::SafeStateOf<S>, StartUp/SafeState.hpp) runs every entry
// of both cores' lists at Panic::beginFatal(), again in the panic dispatch and as Fault::HandlerFor's
// default clean-up - at least twice on a logged fatal path. Any context, any core, interrupts
// maybe masked, maybe on the fault stack, maybe before the part's own init ran: register writes
// and, where the part needs it, a bounded busy wait on the part's own registers - no interrupt,
// no other core, no lock, no log, no allocation. Idempotent. A driver that writes a block that can
// be held in reset checks first that it is out of reset.
struct SafeState {
    using Signature = void();
};

// A declaration, not a call: Health::Check<C> entries in Extends lists, collected by Health::Supervisor through
// Startup::ExtendsOn<HealthCheck> (Health/Health.hpp). Running it would call no-ops.
struct HealthCheck {
    using Signature = void();
};

// Run once, the first time every health check has reported (Health::Supervisor); a boot guard can mark the run
// healthy on it.
struct AllChecksReported {
    using Signature = void();
};
}   // namespace Kvasir::Hook

namespace Kvasir::Startup {
// A profiler around every hook function call and every whole run<Hook>() (Runtime/LoopProfiler.hpp): the image's choice,
// a late hook (the table in kvasir/Port/Hooks.hpp), in the header every translation unit of the firmware includes:
//     template<>
//     struct Kvasir::Startup::HookProfiler<Kvasir::Tag::User> { using type = Kvasir::Profile::HookProfiler<Cfg>; };
// Not specialised: NoHookProfiler, and every call is the plain call it always was.
struct NoHookProfiler {};

template<typename T, typename... Ts>
struct HookProfiler {
    using type = NoHookProfiler;
};

// One function added to one hook.
template<typename THook, auto Fn>
struct Extend {
    using Hook               = THook;
    static constexpr auto fn = Fn;
};

// A declaration, not a peripheral: the application calls Ts' hook functions itself (the header above). Listed like an
// entry; run<Hook>() / runTurn<Hook>() refuse a T that the list also drives through an Extend on that Hook.
template<typename... Ts>
struct DrivenByHand {
    static constexpr bool isStartupEntry
      = true;   // ListRules::hasMarker: it contributes nothing else
    using DrivenByHandComponents = brigand::list<Ts...>;
};

namespace Detail {
    template<typename T>
    struct ExtendsOf {
        using type = brigand::list<>;
    };

    template<typename T>
        requires requires { typename T::Extends; }
    struct ExtendsOf<T> {
        using type = brigand::flatten<brigand::list<typename T::Extends>>;
    };

    // Ps: a list read through Startup::Detail::Entries<...> (Entry.hpp), its groups already their members
    template<typename... Ps>
    using AllExtends = brigand::flatten<brigand::list<typename ExtendsOf<Ps>::type...>>;

    // Whether an Extend belongs to Hook: MPL::Filter<OfHook<Hook>, Extends> is the hook's list, in order.
    template<typename Hook>
    struct OfHook {
        template<typename E>
        static constexpr bool holds = std::is_same_v<typename E::Hook, Hook>;
    };

    // Every component the list declares in a DrivenByHand<...>, in list order.
    template<typename T>
    struct DrivenByHandOf {
        using type = brigand::list<>;
    };

    template<typename T>
        requires requires { typename T::DrivenByHandComponents; }
    struct DrivenByHandOf<T> {
        using type = typename T::DrivenByHandComponents;
    };

    // T is one of List's entries, with an Extend on Hook: run<Hook>() drives it.
    template<typename Hook, typename List>
    struct DrivenByHook {
        template<typename T>
        static constexpr bool holds
          = MPL::contains<T, List>
         && brigand::size<MPL::Filter<OfHook<Hook>, typename ExtendsOf<T>::type>>::value != 0;
    };

    // The components the DrivenByHand<...> entries of List declare and Hook drives too: the rule's offenders.
    template<typename Hook, typename List>
    struct DrivenTwice;

    template<typename Hook, typename... Ps>
    struct DrivenTwice<Hook, brigand::list<Ps...>> {
        using Declared  = brigand::append<brigand::list<>, typename DrivenByHandOf<Ps>::type...>;
        using Offenders = MPL::Filter<DrivenByHook<Hook, brigand::list<Ps...>>, Declared>;
    };

    template<typename List>
    struct FirstOf {
        using type = void;
    };

    template<typename T, typename... Ts>
    struct FirstOf<brigand::list<T, Ts...>> {
        using type = T;
    };
}   // namespace Detail

namespace ListRules {
    // NotDrivenTwice: no component declared in a DrivenByHand<...> is also a listed entry with an Extend on Hook (the
    // header above). Checked where run<Hook>() / runTurn<Hook>() is instantiated; Offender names the first.
    template<typename Hook, typename List>
    struct NotDrivenTwice
      : std::bool_constant<
          brigand::size<
            typename ::Kvasir::Startup::Detail::DrivenTwice<Hook, List>::Offenders>::value
          == 0> {
        using Offender = typename ::Kvasir::Startup::Detail::FirstOf<
          typename ::Kvasir::Startup::Detail::DrivenTwice<Hook, List>::Offenders>::type;
    };

    // The message, built only when the rule fails (KVASIR_STATIC_ASSERT).
    template<typename Hook, typename Offender>
    struct DrivenTwiceText {
        consteval Kvasir::Diagnostic::Text operator()() const {
            Kvasir::Diagnostic::Text t;
            t << "Startup entry ";
            Kvasir::Diagnostic::appendType<Offender>(t);
            t << " is declared in Startup::DrivenByHand<...> and also driven by the hook ";
            Kvasir::Diagnostic::appendType<Hook>(t);
            t << " (its Extends): it runs twice a turn - drop the hand call and the declaration, "
                 "or the Extends";
            return t;
        }
    };
}   // namespace ListRules

namespace Detail {
    template<typename Sig, typename F>
    inline constexpr bool callableAs = false;

    template<typename R, typename... A, typename F>
    inline constexpr bool callableAs<R(A...), F> = std::is_invocable_v<F, A const&...>;

    // The profiler's type, looked up through Port::LateT with an empty pack in templates that depend on the hook: the
    // specialisation is taken at instantiation, so one declared after the first run<Hook>() of a translation unit
    // still wins (as criticalSection's). No deduced return types on the way: they would instantiate early.
    template<typename... Dummy>
    using ProfilerOf = typename Port::LateT<HookProfiler<::Kvasir::Tag::User>, Dummy...>::type;

    template<typename Hook,
             typename... Dummy>
    constexpr bool profiled() {
        using P = ProfilerOf<Dummy...>;
        if constexpr(std::is_same_v<P, NoHookProfiler>) {
            return false;
        } else {
            return P::template profiles<Hook>;
        }
    }

    template<typename Hook,
             typename E,
             typename F,
             typename... Dummy>
    [[gnu::always_inline]] inline std::invoke_result_t<F&> profileEntry(F&& f) {
        using P = ProfilerOf<Dummy...>;
        return P::template entry<Hook, E>(f);
    }

    template<typename Hook,
             typename F,
             typename... Dummy>
    [[gnu::always_inline]] inline std::invoke_result_t<F&> profileTurn(F&& f) {
        using P = ProfilerOf<Dummy...>;
        return P::template turn<Hook>(f);
    }

    // One Extend's function on Hook, or nothing for an Extend on another hook. Want void: run<Hook>(), the result
    // is dropped; Want Kvasir::Turn: runTurn<Hook>(), void is "unknown, so busy", bool "did work", a Turn as it says.
    template<typename Hook,
             typename E,
             typename Want,
             typename... Args>
    [[gnu::always_inline]] inline Want callEntry(Args const&... args) {
        if constexpr(!std::is_same_v<typename E::Hook, Hook>) {
            if constexpr(!std::is_void_v<Want>) { return Kvasir::Turn::idle(); }
        } else {
            static_assert(callableAs<typename Hook::Signature, decltype(E::fn)>,
                          "an Extend's function cannot be called with its hook's Signature");
            if constexpr(std::is_void_v<Want>) {
                if constexpr(!profiled<Hook>()) {
                    E::fn(args...);
                } else {
                    profileEntry<Hook, E>([&] { return E::fn(args...); });
                }
            } else {
                using R = decltype(E::fn(args...));
                static_assert(std::is_void_v<R> || std::is_same_v<R, bool>
                                || std::is_same_v<R, Kvasir::Turn>,
                              "a hook function returns void, bool or Kvasir::Turn");
                auto const call = [&]() -> Kvasir::Turn {
                    if constexpr(std::is_void_v<R>) {
                        E::fn(args...);
                        return Kvasir::Turn::busy();
                    } else if constexpr(std::is_same_v<R, bool>) {
                        return E::fn(args...) ? Kvasir::Turn::busy() : Kvasir::Turn::idle();
                    } else {
                        return E::fn(args...);
                    }
                };
                if constexpr(!profiled<Hook>()) {
                    return call();
                } else {
                    return profileEntry<Hook, E>(call);
                }
            }
        }
    }

    // Every Extend of the list on Hook, in list order; Want Kvasir::Turn folds the Turns (the earliest wins).
    template<typename Hook,
             typename Want,
             typename... Es,
             typename... Args>
    [[gnu::always_inline]] inline Want callEntries(brigand::list<Es...>*,
                                                   Args const&... args) {
        if constexpr(std::is_void_v<Want>) {
            (callEntry<Hook, Es, void>(args...), ...);
        } else {
            Kvasir::Turn t = Kvasir::Turn::idle();
            ((t = t | callEntry<Hook, Es, Kvasir::Turn>(args...)), ...);   // comma fold: list order
            return t;
        }
    }

    // The one dispatch of a hook over a list of entries: run<Hook>() (Want void) and runTurn<Hook>() (Want
    // Kvasir::Turn). Checks the call against Hook::Signature and the list against NotDrivenTwice, then calls every
    // function Ps... added to Hook, in list order, under the hook profiler where one profiles Hook.
    template<typename Hook,
             typename Want,
             typename... Ps,
             typename... Args>
    [[gnu::always_inline]] inline Want dispatch(brigand::list<Ps...>*,
                                                Args const&... args) {
        if constexpr(std::is_void_v<Want>) {
            static_assert(
              std::is_invocable_v<std::add_pointer_t<typename Hook::Signature>, Args const&...>,
              "Startup::run<Hook>(args...) does not match Hook::Signature");
        } else {
            static_assert(
              std::is_invocable_v<std::add_pointer_t<typename Hook::Signature>, Args const&...>,
              "Startup::runTurn<Hook>(args...) does not match Hook::Signature");
        }
        using Twice = ListRules::NotDrivenTwice<Hook, brigand::list<Ps...>>;
        KVASIR_STATIC_ASSERT(Twice::value,
                             (ListRules::DrivenTwiceText<Hook, typename Twice::Offender>));
        if constexpr(!profiled<Hook>()) {
            return callEntries<Hook, Want>(static_cast<AllExtends<Ps...>*>(nullptr), args...);
        } else {
            return profileTurn<Hook>([&] {
                return callEntries<Hook, Want>(static_cast<AllExtends<Ps...>*>(nullptr), args...);
            });
        }
    }
}   // namespace Detail

// Every Extend a brigand::list of entries adds, in list order: `ExtendsOfList<S::LocalPeripherals>` for a Startup or
// SecondaryCore S (Runtime/Executor.hpp links the Periodic ones from it).
template<typename List>
using ExtendsOfList = MPL::Expand<Detail::AllExtends, List>;

// Whether Extend E adds a function to Hook that returns void: it keeps a sleeping loop awake (Executor's boot report).
template<typename Hook, typename E>
inline constexpr bool returnsVoidOn = [] {
    if constexpr(!std::is_same_v<typename E::Hook, Hook>) {
        return false;
    } else {
        return std::is_void_v<std::invoke_result_t<decltype(E::fn)>>;
    }
}();
}   // namespace Kvasir::Startup
