#pragma once
// Bounded waits on a register: `waitUntil<Bound, Policy>(condition)`. The bound (a datasheet time, a poll count or a
// caller's deadline) sits next to its citation; the policy says what a timeout does:
//
//     // SAM D21 Table 37-58: tLOCK max 2 ms at f_IN = 32 kHz
//     waitUntil<Bound::Microseconds<2'000, CpuMaxHz>>(isSet(KSR::DPLLSTATUS::clkrdy));
//     // the caller handles the failure
//     if(!waitUntil<Bound::Polls<1'000>, OnTimeout::Report>(isSet(R::flag))) { ... }
//
// The default policy, OnTimeout::Unbounded, compiles to a plain `while(!cond) {}`. An application turns timeouts on
// once, in the header with its Startup list (a stuck wait then panics with Panic::Cause::timeout):
//
//     template<> inline constexpr auto Kvasir::Register::injectedWaitPolicy<> = Kvasir::Register::OnTimeout::Panic{};
//
// A RAM function that runs with the flash unreadable may use only Count / Report / Unbounded: Panic reaches flash,
// and check_ram_funcs.py refuses the link.
#include "kvasir/Register/Register.hpp"
#include "kvasir/Util/Panic.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace Kvasir::Register {

#ifndef KVASIR_WAIT_POLL_MARGIN
    #define KVASIR_WAIT_POLL_MARGIN 2
#endif
inline constexpr std::uint32_t PollMargin = KVASIR_WAIT_POLL_MARGIN;
// A poll is at least a load, a compare and a taken branch: no fewer cycles than this on a Cortex-M0+, M33 or M4.
// Fewer assumed cycles mean more polls and a later timeout, never an early one.
inline constexpr std::uint32_t MinCyclesPerPoll = 4;

struct WaitResult {
    std::uint32_t polls{};   // reads it took (0 under Unbounded)
    bool          timedOut{};

    [[nodiscard]] constexpr explicit operator bool() const { return !timedOut; }
};

// ---- conditions ----
template<typename Field>
struct IsSet {
    Field field;
};

template<typename Field>
struct IsClear {
    Field field;
};

template<typename Field>
constexpr IsSet<Field> isSet(Field f) {
    return {f};
}

template<typename Field>
constexpr IsClear<Field> isClear(Field f) {
    return {f};
}

namespace Detail {
    // a callable predicate: no register to name
    template<typename C>
    struct CondTraits {
        static constexpr std::uint32_t address = 0;

        [[gnu::always_inline]] static bool holds(C const& c) { return static_cast<bool>(c()); }
    };

    // The same reads the loops they replace make: `get<0>(apply(read(f)))` against 0.
    template<typename F>
    struct CondTraits<IsSet<F>> {
        static constexpr std::uint32_t address = GetAddress<F>::value;

        [[gnu::always_inline]] static bool holds(IsSet<F> const& c) {
            return get<0>(apply(read(c.field))) != 0;
        }
    };

    template<typename F>
    struct CondTraits<IsClear<F>> {
        static constexpr std::uint32_t address = GetAddress<F>::value;

        [[gnu::always_inline]] static bool holds(IsClear<F> const& c) {
            return get<0>(apply(read(c.field))) == 0;
        }
    };

    template<typename TField, typename TField::DataType V>
    struct CondTraits<FieldValue<TField, V>> {
        static constexpr std::uint32_t address = GetAddress<TField>::value;

        [[gnu::always_inline]] static bool holds(FieldValue<TField,
                                                            V> fv) {
            return fieldEquals(fv);
        }
    };
}   // namespace Detail

// ---- bounds ----
namespace Bound {
    template<std::uint32_t N>
    struct Polls {
        static_assert(N > 0,
                      "a bound of no polls");
        static constexpr std::uint32_t polls = N;
    };

    // A datasheet time, as polls at the highest clock the core may run at when the wait runs (CpuHz, the chip
    // package's ceiling): a slower clock (at boot, from the ring oscillator) only makes the real time longer.
    template<std::uint64_t Us, std::uint64_t CpuHz>
    struct Microseconds {
        static constexpr std::uint64_t raw
          = (Us * CpuHz / 1'000'000ULL + MinCyclesPerPoll - 1) / MinCyclesPerPoll;
        static_assert(raw > 0,
                      "a bound of no polls");
        static_assert(raw * PollMargin < 0xFFFF'FFFFULL,
                      "the bound is too long for a poll count: use Bound::Until<Clock>");
        static constexpr std::uint32_t polls = static_cast<std::uint32_t>(raw * PollMargin);
    };

    // A caller's deadline on a running clock (C::now(), C::time_point). Polls are counted against Ceiling too: a
    // SysTick clock with interrupts masked stops after two reloads, and the deadline alone would never expire.
    template<typename C, std::uint32_t Ceiling = 0xFFFF'FFFFU>
    struct Until {
        using clock                            = C;
        static constexpr std::uint32_t ceiling = Ceiling;
        typename C::time_point         end;
    };
}   // namespace Bound

// ---- policies ----
struct WaitSite {
    std::uint32_t address;   // the polled register; 0 for a callable
};

template<typename P>
concept WaitPolicy = requires(WaitSite s, std::uint32_t n) {
    { P::bounded } -> std::convertible_to<bool>;
    P::timedOut(s);
    P::finished(s, n);
};

namespace Detail {
    [[noreturn,
      gnu::noinline,
      gnu::cold]] inline void
    waitPanic(std::uint32_t address) {
        // the return address is in the function the wait was inlined into: the site
        Panic::raiseAt(
          Panic::Cause::timeout,
          static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),
          address);
    }
}   // namespace Detail

namespace OnTimeout {
    // the plain loop; the default
    struct Unbounded {
        static constexpr bool bounded = false;

        static void timedOut(WaitSite) {}

        static void finished(WaitSite,
                             std::uint32_t) {}
    };

    // the caller handles the WaitResult
    struct Report {
        static constexpr bool bounded = true;

        static void timedOut(WaitSite) {}

        static void finished(WaitSite,
                             std::uint32_t) {}
    };

    // carry on and count it; safe in a RAM function
    struct Count {
        static constexpr bool bounded = true;
        static inline std::uint32_t volatile timeouts{};

        static void timedOut(WaitSite) { timeouts = timeouts + 1; }

        static void finished(WaitSite,
                             std::uint32_t) {}
    };

    // Panic::Cause::timeout, the detail the register's address
    struct Panic {
        static constexpr bool bounded = true;

        // inlined, so waitPanic()'s return address is the waiting code, not this function
        [[noreturn,
          gnu::always_inline]] static void
        timedOut(WaitSite s) {
            Detail::waitPanic(s.address);
        }

        static void finished(WaitSite,
                             std::uint32_t) {}
    };
}   // namespace OnTimeout

template<typename...>
inline constexpr auto injectedWaitPolicy = OnTimeout::Unbounded{};

struct UseInjected {};

template<typename BoundT,
         typename PolicyT = UseInjected,
         typename... Dummy,
         typename Cond>
    requires(sizeof...(Dummy) == 0)
[[gnu::always_inline]] inline WaitResult waitUntil(Cond const&   cond,
                                                   BoundT const& bound = BoundT{}) {
    using P = std::conditional_t<std::is_same_v<PolicyT, UseInjected>,
                                 std::remove_cvref_t<decltype(injectedWaitPolicy<Dummy...>)>,
                                 PolicyT>;
    static_assert(WaitPolicy<P>,
                  "a wait policy: bounded, timedOut(WaitSite), finished(WaitSite, n)");
    using T = Detail::CondTraits<Cond>;
    constexpr WaitSite site{T::address};

    if constexpr(!P::bounded) {
        static_cast<void>(bound);
        while(!T::holds(cond)) {}
        return {};
    } else if constexpr(requires { BoundT::polls; }) {
        static_cast<void>(bound);
        for(std::uint32_t n = 1; n <= BoundT::polls; ++n) {
            if(T::holds(cond)) {
                P::finished(site, n);
                return {n, false};
            }
        }
        P::timedOut(site);
        return {BoundT::polls, true};
    } else {
        std::uint32_t n = 0;
        while(!T::holds(cond)) {
            if(++n >= BoundT::ceiling || !(BoundT::clock::now() < bound.end)) {
                P::timedOut(site);
                return {n, true};
            }
        }
        P::finished(site, n + 1);
        return {n + 1, false};
    }
}
}   // namespace Kvasir::Register
