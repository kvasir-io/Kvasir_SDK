#pragma once
// Named hooks a Startup list's peripherals add functions to, run by whoever owns the hook:
//
//     struct Master {
//         static void handler();   // timeouts, completion polling
//         using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Master::handler>;
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
// A loop that sleeps (Util/Executor.hpp) runs Startup::runTurn<Hook>() instead: the same calls, and each function
// may say when it next needs the loop by what it returns - Kvasir::Turn (Turn::within(d), Turn::idle()), bool ("did
// work": busy if true, idle if false) or void (unknown, so busy: a function never converted keeps the loop awake).
// The fold keeps the earliest. run<Hook>() still drops the values.
//
// What this cannot enforce: that the application calls Startup::run<Hook::MainLoop>() at all.
// A driver that gains an Extends must not also be called by hand in a firmware that runs the
// hook, or it runs twice a turn.

#include "kvasir/Mpl/brigand.hpp"

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
// halt: from the fault handler's clean-up, a panic handler. Interrupts may be masked and the
// stack may be the fault stack: register writes only, no waiting, no logging.
struct SafeState {
    using Signature = void();
};

// A declaration, not a call: Health::Check<C> entries in Extends lists, collected by Health::Supervisor through
// Startup::ExtendsOn<HealthCheck> (Util/Health.hpp). Running it would call no-ops.
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
// A profiler around every hook function call and every whole run<Hook>() (Util/LoopProfiler.hpp), injected like a
// critical-section policy, in the header every translation unit of the firmware includes:
//     template<> inline constexpr auto Kvasir::Startup::injectedHookProfiler<> = Kvasir::Profile::HookProfiler<Cfg>{};
// Not specialised: NoHookProfiler, and every call is the plain call it always was.
struct NoHookProfiler {};

template<typename...>
inline constexpr auto injectedHookProfiler = NoHookProfiler{};

// One function added to one hook.
template<typename THook, auto Fn>
struct Extend {
    using Hook               = THook;
    static constexpr auto fn = Fn;
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

    template<typename... Ps>
    using AllExtends = brigand::flatten<brigand::list<typename ExtendsOf<Ps>::type...>>;

    // The Extends of Hook among Es..., in order.
    template<typename Hook, typename List>
    struct FilterExtends;

    template<typename Hook, typename... Es>
    struct FilterExtends<Hook, brigand::list<Es...>> {
        using type = brigand::flatten<
          brigand::list<std::conditional_t<std::is_same_v<typename Es::Hook, Hook>,
                                           brigand::list<Es>,
                                           brigand::list<>>...>>;
    };

    template<typename List>
    struct AllExtendsOfList;

    template<typename... Ps>
    struct AllExtendsOfList<brigand::list<Ps...>> {
        using type = AllExtends<Ps...>;
    };

    template<typename Sig, typename F>
    inline constexpr bool callableAs = false;

    template<typename R, typename... A, typename F>
    inline constexpr bool callableAs<R(A...), F> = std::is_invocable_v<F, A const&...>;

    // The profiler's type, looked up through an empty pack in templates that depend on the hook: the
    // specialisation is taken at instantiation, so one declared after the first run<Hook>() of a translation unit
    // still wins (as criticalSection's). No deduced return types on the way: they would instantiate early.
    template<typename Hook,
             typename... Dummy>
    constexpr bool profiled() {
        using P = std::remove_cvref_t<decltype(injectedHookProfiler<Dummy...>)>;
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
        using P = std::remove_cvref_t<decltype(injectedHookProfiler<Dummy...>)>;
        return P::template entry<Hook, E>(f);
    }

    template<typename Hook,
             typename F,
             typename... Dummy>
    [[gnu::always_inline]] inline std::invoke_result_t<F&> profileTurn(F&& f) {
        using P = std::remove_cvref_t<decltype(injectedHookProfiler<Dummy...>)>;
        return P::template turn<Hook>(f);
    }

    template<typename Hook,
             typename E,
             typename... Args>
    [[gnu::always_inline]] inline void callIfOn(Args const&... args) {
        if constexpr(std::is_same_v<typename E::Hook, Hook>) {
            static_assert(callableAs<typename Hook::Signature, decltype(E::fn)>,
                          "an Extend's function cannot be called with its hook's Signature");
            if constexpr(!profiled<Hook>()) {
                E::fn(args...);
            } else {
                profileEntry<Hook, E>([&] { return E::fn(args...); });
            }
        }
    }

    template<typename Hook,
             typename... Es,
             typename... Args>
    [[gnu::always_inline]] inline void runHook(brigand::list<Es...>*,
                                               Args const&... args) {
        (callIfOn<Hook, Es>(args...), ...);
    }

    // void: unknown, so busy. bool: "did work". Turn: as it says.
    template<typename Hook,
             typename E,
             typename... Args>
    [[gnu::always_inline]] inline Kvasir::Turn turnIfOn(Args const&... args) {
        if constexpr(!std::is_same_v<typename E::Hook, Hook>) {
            return Kvasir::Turn::idle();
        } else {
            static_assert(callableAs<typename Hook::Signature, decltype(E::fn)>,
                          "an Extend's function cannot be called with its hook's Signature");
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

    template<typename Hook,
             typename... Es,
             typename... Args>
    [[gnu::always_inline]] inline Kvasir::Turn runTurn(brigand::list<Es...>*,
                                                       Args const&... args) {
        Kvasir::Turn t = Kvasir::Turn::idle();
        ((t = t | turnIfOn<Hook, Es>(args...)), ...);   // comma fold: list order, as runHook
        return t;
    }

    template<typename Hook,
             typename... Ps,
             typename... Args>
    [[gnu::always_inline]] inline Kvasir::Turn runTurnOf(brigand::list<Ps...>*,
                                                         Args const&... args) {
        static_assert(
          std::is_invocable_v<std::add_pointer_t<typename Hook::Signature>, Args const&...>,
          "Startup::runTurn<Hook>(args...) does not match Hook::Signature");
        if constexpr(!profiled<Hook>()) {
            return runTurn<Hook>(static_cast<AllExtends<Ps...>*>(nullptr), args...);
        } else {
            return profileTurn<Hook>(
              [&] { return runTurn<Hook>(static_cast<AllExtends<Ps...>*>(nullptr), args...); });
        }
    }

    // The void-returning functions on Hook among Ps... (they keep a sleeping loop awake), for a boot report.
    template<typename Hook, typename E>
    inline constexpr bool returnsVoidOn = [] {
        if constexpr(!std::is_same_v<typename E::Hook, Hook>) {
            return false;
        } else {
            return std::is_void_v<std::invoke_result_t<decltype(E::fn)>>;
        }
    }();

    // Every function Ps... added to Hook, in list order; checks the call against the Signature.
    template<typename Hook,
             typename... Ps,
             typename... Args>
    [[gnu::always_inline]] inline void runHookOf(brigand::list<Ps...>*,
                                                 Args const&... args) {
        static_assert(
          std::is_invocable_v<std::add_pointer_t<typename Hook::Signature>, Args const&...>,
          "Startup::run<Hook>(args...) does not match Hook::Signature");
        if constexpr(!profiled<Hook>()) {
            runHook<Hook>(static_cast<AllExtends<Ps...>*>(nullptr), args...);
        } else {
            profileTurn<Hook>(
              [&] { runHook<Hook>(static_cast<AllExtends<Ps...>*>(nullptr), args...); });
        }
    }
}   // namespace Detail
}   // namespace Kvasir::Startup
