#pragma once
// The image's safe state as one function: every Extend<Hook::SafeState, &f> of both cores' Startup lists, in list
// order. A product names it as its fatal start (Runtime/Panic.hpp), one line after `using Startup = ...`:
//
//     template<>
//     struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
//         using type = Kvasir::Startup::SafeStateOf<::Startup>;
//     };
//
// `::Startup`, the image's list: inside the specialisation a plain `Startup` is the namespace Kvasir::Startup.
//
// It then runs at every Panic::beginFatal() (before the fatal path's log line), again in the panic dispatch after
// interrupts are masked, and as Fault::HandlerFor's default clean-up (Options::cleanUp). Start<S> refuses a list that
// extends Hook::SafeState while the fatal start does not run it (Detail::safeStateWired below).
//
// The calls are direct: no hook profiler on a fatal path. Out of line and cold, so the many inlined beginFatal()
// sites cost one call each, not one copy of the list.
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Runtime/Panic.hpp"

#include <type_traits>

namespace Kvasir::Startup {
namespace Detail {
    template<typename... Es>
    [[gnu::always_inline]] inline void runSafeStateOf(brigand::list<Es...>*) {
        (Es::fn(), ...);
    }

    template<typename List>
    inline constexpr bool emptyList = false;

    template<>
    inline constexpr bool emptyList<brigand::list<>> = true;
}   // namespace Detail

/// The safe state of the Startup S: S::ExtendsOn<Hook::SafeState> (both cores' lists) in one function.
template<typename S>
struct SafeStateOf {
    [[gnu::noinline,
      gnu::cold]] static void
    operator()() {
        Detail::runSafeStateOf(
          static_cast<typename S::template ExtendsOn<Hook::SafeState>*>(nullptr));
    }
};

namespace Detail {
    /// Start<S>'s check: a list that extends Hook::SafeState needs a fatal start that runs something - the
    /// SafeStateOf<S> line, or a start of the application's own that calls it (FatalStart = the image's
    /// Panic::FatalStartReaction).
    template<typename S,
             typename FatalStart>
    consteval bool safeStateWired() {
        static_assert(
          emptyList<typename S::template ExtendsOn<Hook::SafeState>>
            || !std::is_same_v<FatalStart, Panic::NoFatalStart>,
          "the Startup list extends Kvasir::Hook::SafeState, but the fatal start does not run it: "
          "add "
          "`template<> struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> { "
          "using type = Kvasir::Startup::SafeStateOf<::Startup>; };` after `using Startup = ...` "
          "(kvasir/StartUp/SafeState.hpp)");
        return true;
    }
}   // namespace Detail
}   // namespace Kvasir::Startup
