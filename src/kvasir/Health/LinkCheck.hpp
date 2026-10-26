#pragma once
// A health check fed from a part's link (Util/Link.hpp), or from any predicate, listed in the Startup list instead of
// glue in main:
//
//     struct DisplayCheck {
//         static constexpr std::string_view name = "display";
//         static constexpr auto maxInterval      = std::chrono::seconds{10};
//     };
//     using Startup = Kvasir::Startup::Startup<..., Kvasir::Health::LinkCheck<Display, DisplayCheck>, ...,
//                                              Kvasir::Health::Service<>>;
//
// Every main-loop turn (Hook::MainLoop, the entry's turn()) it reads Part::link() and checks in while the part is
// answering, or offline (not talked to on purpose; `static constexpr bool offlineBeats = false;` in the check Config
// opts out). `starting` and `absent` do not beat: a part that stays so for longer than maxInterval (after
// graceAfterBoot) starves the check - the Config's onStarved if it has one, then the supervisor's policy. The entry
// adds no onStarved of its own: the part's own recovery (probing, its retry, a restart) is rung 1-2 and runs anyway;
// what Health does about a part that stays away is the Config's.
//
// The three forms:
//   LinkCheck<Part, C>          Part has `static Link link()` (the static displays, the stores)
//   LinkCheck<Of<object>, C>    an object of static storage duration with a `link()` member
//   checkInIf<C>(x.link())      a local of main: one call in the loop, no condition; Check<C> still goes into an
//                               entry's Extends (`struct H { using Extends = brigand::list<Check<C>>; };` listed):
//                               Check<C> is no Startup entry itself
// and the general one, PolledCheck<&predicate, C>: a `bool()` the product writes ("every store written").
//
// Every part's link() returns to answering by itself (the header comment of each says what brings it back). A link
// that latches would turn one fault into a reset loop: never list a LinkCheck on one. chip_rp_common's PioQspi is
// such a part - its link stays absent until its caller's clearError() -, so the check goes on the panel driver that
// retries it (DcsPanel), never on the transport.
//
// Needs Startup::run<Hook::MainLoop>() in the loop (a hung loop starves the check too). Includes only the check's
// half of Health (Health/Check.hpp): the supervisor stays where the product names it.
#include "kvasir/Health/Check.hpp"
#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Link.hpp"

#include <concepts>

namespace Kvasir::Health {
namespace Detail {
    template<typename C>
    consteval bool offlineBeats() {
        return static_cast<bool>(KVASIR_CONFIG_OR(C, offlineBeats, true));
    }

    /// Whether this link is a beat of check C.
    template<typename C>
    [[gnu::always_inline]] constexpr bool beats(Link l) {
        return l == Link::answering || (offlineBeats<C>() && l == Link::offline);
    }
}   // namespace Detail

/// A part that answers (or is offline, unless C opts out) checks in.
template<CheckConfig C>
[[gnu::always_inline]] inline void checkInIf(Link l) {
    if(Detail::beats<C>(l)) { checkIn<C>(); }
}

/// An object as a part: `LinkCheck<Health::Of<display>, C>` (static storage duration).
template<auto const& Obj>
struct Of {
    [[gnu::always_inline]] static Link link() { return Obj.link(); }
};

template<typename P>
concept HasLink = requires {
    { P::link() } -> std::same_as<Link>;
};

/// A check fed by a product predicate `bool()`: true is a beat.
template<auto Pred, CheckConfig C>
    requires requires {
        { Pred() } -> std::convertible_to<bool>;
    }
struct PolledCheck {
    static void turn() {
        if(Pred()) { checkIn<C>(); }
    }

    using Extends
      = brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &PolledCheck::turn>,
                      Check<C>>;
};

/// A check fed by a part's link.
template<typename Part, CheckConfig C>
struct LinkCheck {
    static_assert(HasLink<Part>,
                  "Health::LinkCheck<Part, C>: Part needs `static Kvasir::Link link()` - an object "
                  "goes through "
                  "Health::Of<object>, a local of main through Health::checkInIf<C>(x.link())");

    static void turn() { checkInIf<C>(Part::link()); }

    using Extends
      = brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &LinkCheck::turn>, Check<C>>;
};
}   // namespace Kvasir::Health
