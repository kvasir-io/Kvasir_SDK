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
// What this cannot enforce: that the application calls Startup::run<Hook::MainLoop>() at all.
// A driver that gains an Extends must not also be called by hand in a firmware that runs the
// hook, or it runs twice a turn.

#include "kvasir/Mpl/brigand.hpp"

#include <type_traits>

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
}   // namespace Kvasir::Hook

namespace Kvasir::Startup {
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

    template<typename Sig, typename F>
    inline constexpr bool callableAs = false;

    template<typename R, typename... A, typename F>
    inline constexpr bool callableAs<R(A...), F> = std::is_invocable_v<F, A const&...>;

    template<typename Hook,
             typename E,
             typename... Args>
    [[gnu::always_inline]] inline void callIfOn(Args const&... args) {
        if constexpr(std::is_same_v<typename E::Hook, Hook>) {
            static_assert(callableAs<typename Hook::Signature, decltype(E::fn)>,
                          "an Extend's function cannot be called with its hook's Signature");
            E::fn(args...);
        }
    }

    template<typename Hook,
             typename... Es,
             typename... Args>
    [[gnu::always_inline]] inline void runHook(brigand::list<Es...>*,
                                               Args const&... args) {
        (callIfOn<Hook, Es>(args...), ...);
    }

    // Every function Ps... added to Hook, in list order; checks the call against the Signature.
    template<typename Hook,
             typename... Ps,
             typename... Args>
    [[gnu::always_inline]] inline void runHookOf(brigand::list<Ps...>*,
                                                 Args const&... args) {
        static_assert(
          std::is_invocable_v<std::add_pointer_t<typename Hook::Signature>, Args const&...>,
          "Startup::run<Hook>(args...) does not match Hook::Signature");
        runHook<Hook>(static_cast<AllExtends<Ps...>*>(nullptr), args...);
    }
}   // namespace Detail
}   // namespace Kvasir::Startup
