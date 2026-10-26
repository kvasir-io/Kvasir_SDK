#pragma once

// Clocks as Startup resources (kvasir/StartUp/Resources.hpp), the vocabulary every chip family
// shares. Every driver config carries a `clockSpeed`, and nothing else relates it to the clock
// its block counts: a config with 150 MHz on a 200 MHz build gives a wrong baud rate and
// compiles. So the clock settings *provide* each clock at the rate they program, every driver
// *claims* the clock its block runs from at the rate its config says, and a number nothing
// provides is refused by Startup (rule 3: "claimed but not provided").
//
//     struct ClockSettings {
//         using Provides = brigand::list<Kvasir::Hal::Clk<Kvasir::Clocks::ClkPeri, 150'000'000>>;
//         ...
//     };
//     struct UartDriver {
//         using Claims = Kvasir::Hal::Claim<Kvasir::Clocks::ClkPeri, Config::clockSpeed>;
//     };
//
// The clock *tags* (`ClkPeri`, `ClkUsb`, `ClkSercomCore<n>`, ...) are chip data and live in
// each chip family's Clocks.hpp, derived from `ClockTag`. Every claim of a tag needs a provider:
// a ClockSettings states its `Provides`, and one without them does not build once a driver
// claims a clock - on every family (a SAM block without its GCLK never runs; an RP block on a
// clk_peri the tree does not program counts the wrong rate).

#include "kvasir/Mpl/brigand.hpp"
#include "kvasir/StartUp/Resources.hpp"

#include <cstddef>

namespace Kvasir { namespace Hal {
    struct ClockTag {
        static constexpr bool sharedClaim = true;    // every driver on a clock claims it
        static constexpr bool coreLocal   = false;   // the tree serves both cores
    };

    template<typename Which, auto Hz>
    using Clk = Startup::Resource<Which, Hz>;

    // `using Claims = Kvasir::Hal::Claim<Kvasir::Clocks::ClkPeri, Config::clockSpeed>;`
    template<typename Which, auto Hz>
    using Claim = brigand::list<Clk<Which, Hz>>;

    // ---- reading a rate back off a Provides list -----------------------------------------
    //
    // A config that says "this block counts clk_ref" wants the number the tree programs
    // clk_ref to, not a copy of it: `hzOf<ClkRef>(Provides{})` is the crystal on the RP's
    // DefaultProvides. A clock the list does not provide (or provides twice) is a build
    // error, which is what keeps that config from silently becoming a number.
    namespace Detail {
        template<typename Which,
                 typename R>
        consteval bool providesClock() {
            if constexpr(Startup::isResourceOf<R, Which>) {
                return Startup::ResourceIds<R>::ids.size() == 1;
            } else {
                return false;
            }
        }

        template<typename Which,
                 typename R>
        consteval unsigned long long clockHz() {
            if constexpr(providesClock<Which, R>()) {
                return Startup::ResourceIds<R>::ids[0];
            } else {
                return 0;
            }
        }
    }   // namespace Detail

    /// How many entries of the list provide `Which` (1 on a well-formed tree).
    template<typename Which,
             typename... Resources>
    consteval std::size_t providersOf(brigand::list<Resources...>) {
        return ((Detail::providesClock<Which, Resources>() ? std::size_t{1} : std::size_t{0}) + ...
                + std::size_t{0});
    }

    /// The rate the list provides `Which` at.
    template<typename Which,
             typename... Resources>
    consteval unsigned long long hzOf(brigand::list<Resources...> provides) {
        static_assert(providersOf<Which>(provides) == 1,
                      "the clock tree provides no such clock (or more than one)");
        return (Detail::clockHz<Which, Resources>() + ... + 0ULL);
    }

    /// The same with the list as a type: `hzOf<ClkRef, Provides>()`.
    template<typename Which,
             typename Provides>
    consteval unsigned long long hzOf() {
        return hzOf<Which>(Provides{});
    }
}}   // namespace Kvasir::Hal
