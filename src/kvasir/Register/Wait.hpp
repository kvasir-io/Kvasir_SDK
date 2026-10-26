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
// once, in the header with its Startup list (a stuck wait then panics with Panic::Cause::timeout), through the late
// hook WaitPolicy (the table in kvasir/Port/Hooks.hpp):
//
//     template<>
//     struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> { using type = Kvasir::Register::OnTimeout::Panic; };
//
// A datasheet time at the chip's ceiling clock is `MicrosecondsBound<Us>`: each chip package specialises the hook
// `CpuHzCeiling<Tag::User>` in its chip/WaitBounds.hpp, and every chip bound comes through that header.
//
// OnTimeout::Call<&fn> calls the application's `void fn(std::uint32_t address)` on a timeout and carries on.
// A RAM function that runs with the flash unreadable may use only Count / Report / Unbounded (or a Call whose fn is a
// RAM function): Panic reaches flash, and check_ram_funcs.py refuses the link.
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Register/Register.hpp"
#include "kvasir/Runtime/Panic.hpp"

#include <concepts>
#include <cstdint>
#include <type_traits>

namespace Kvasir::Register {

#ifndef KVASIR_WAIT_POLL_MARGIN
    #define KVASIR_WAIT_POLL_MARGIN 2
#endif
inline constexpr std::uint32_t pollMargin = KVASIR_WAIT_POLL_MARGIN;
// A poll is at least a load, a compare and a taken branch: no fewer cycles than this on a Cortex-M0+, M33 or M4.
// Cortex-M0+: LDR 1 (single-cycle I/O port) or 2, CMP 1, B<cc> taken 2 (TRM DDI0484C Table 3-1, md l.837, l.868,
// l.907). Cortex-M4: LDR 2, CMP 1, B<cc> taken 1 + P with P >= 1 (TRM 100166 r0p1 3.3.1 Table 3-1, md l.1241,
// l.1296, l.1331, l.1384). Cortex-M33 (no timing in its TRM; the RP2350 datasheet's): LDR 1, the compare 1, a taken
// B<cc> after a flag-setting instruction 2 (3.7.4.9.5, 3.7.4.9.7, RP2350_Datasheet.md l.6426, l.6482, l.6516).
// Fewer assumed cycles mean more polls and a later timeout, never an early one.
inline constexpr std::uint32_t minCyclesPerPoll = 4;

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
        static constexpr std::uint32_t address = RegisterTraits<F>::address;

        [[gnu::always_inline]] static bool holds(IsSet<F> const& c) {
            return get<0>(apply(read(c.field))) != 0;
        }
    };

    template<typename F>
    struct CondTraits<IsClear<F>> {
        static constexpr std::uint32_t address = RegisterTraits<F>::address;

        [[gnu::always_inline]] static bool holds(IsClear<F> const& c) {
            return get<0>(apply(read(c.field))) == 0;
        }
    };

    template<typename TField, typename TField::DataType V>
    struct CondTraits<FieldValue<TField, V>> {
        static constexpr std::uint32_t address = RegisterTraits<TField>::address;

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
        static_assert(CpuHz > 0,
                      "no chip package specialised Kvasir::Register::CpuHzCeiling<Tag::User> "
                      "(chip/WaitBounds.hpp)");
        static constexpr std::uint64_t raw
          = (Us * CpuHz / 1'000'000ULL + minCyclesPerPoll - 1) / minCyclesPerPoll;
        static_assert(raw > 0,
                      "a bound of no polls");
        static_assert(raw * pollMargin < 0xFFFF'FFFFULL,
                      "the bound is too long for a poll count: use Bound::Until<Clock>");
        static constexpr std::uint32_t polls = static_cast<std::uint32_t>(raw * pollMargin);
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

// The fastest clock the core may run at when a wait runs: the late hook CpuHzCeiling (its primary in
// kvasir/Port/Hooks.hpp). Each chip package specialises it in its chip/WaitBounds.hpp, before anything names
// MicrosecondsBound (a chip's bounds are named at namespace scope, not looked up late):
//
//     template<> struct Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User> {
//         static constexpr std::uint64_t value = 100'000'000;   // the chip's data sheet maximum
//     };
//
// A slower clock only makes the real time of a bound longer. The primary's 0 is Bound::Microseconds' named error.

// A datasheet time as polls at the chip's ceiling clock.
template<std::uint64_t Us, typename... Dummy>
    requires(sizeof...(Dummy) == 0)
using MicrosecondsBound
  = Bound::Microseconds<Us,
                        ::Kvasir::Port::LateT<CpuHzCeiling<::Kvasir::Tag::User>, Dummy...>::value>;

// ---- policies ----
struct WaitSite {
    std::uint32_t address;   // the polled register; 0 for a callable
};

template<typename P>
concept WaitPolicyShape = requires(WaitSite s, std::uint32_t n) {
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
        Panic::raiseAt(Panic::Cause::timeout, KVASIR_DETAIL_CALLER_PC(), address);
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

    // bounded, and a timeout does nothing more: the WaitResult goes back as under Report, which no caller reads
    struct Ignore {
        static constexpr bool bounded = true;

        static void timedOut(WaitSite) {}

        static void finished(WaitSite,
                             std::uint32_t) {}
    };

    // A line for the timeouts, written from the main loop, never from the wait: a wait runs before logging works (the
    // chip's clock set-up, before initMemory() - a timeout there is not kept: initMemory() clears the record), in an
    // interrupt, or in a RAM function with the flash unreadable - and this header is read by the log's own config
    // header, which must not reach uc_log. timedOut() only stores the register's address and counts (one store each, as
    // Count; safe in a RAM function); the line comes from Register::TimeoutReport (kvasir/Register/WaitLog.hpp), a
    // Hook::MainLoop entry for the Startup list: one warning per batch of new timeouts. Without it, the count only.
    struct Log {
        static constexpr bool bounded = true;
        static inline std::uint32_t volatile timeouts{};
        static inline std::uint32_t volatile lastAddress{};

        static void timedOut(WaitSite s) {
            lastAddress = s.address;
            timeouts    = timeouts + 1;
        }

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

    // Counts timeouts in a row (a wait that holds starts the count again) and panics at the Limit-th, as Panic does
    // (Panic::Cause::timeout, the register's address as the detail); the ones before carry on with the WaitResult, as
    // Report. Safe in a RAM function as Panic is (that is: not with the flash unreadable). The chip's clock set-up runs
    // before initMemory(): its waits count too, and initMemory() clears that count with the RAM.
    template<std::uint32_t Limit>
    struct PanicAfter {
        static_assert(Limit >= 1,
                      "PanicAfter<0> would never let a wait time out: use OnTimeout::Panic");
        static constexpr bool          bounded = true;
        static constexpr std::uint32_t limit   = Limit;
        static inline std::uint32_t volatile inARow{};

        [[gnu::always_inline]] static void timedOut(WaitSite s) {
            inARow = inARow + 1;
            if(inARow >= Limit) { Detail::waitPanic(s.address); }
        }

        static void finished(WaitSite,
                             std::uint32_t) {
            inARow = 0;
        }
    };

    // the application's function, `void fn(std::uint32_t address)` (the polled register; 0 for a callable), then
    // carry on with the WaitResult; in a RAM function only a fn that is a RAM function itself
    template<auto Fn>
    struct Call {
        static constexpr bool bounded = true;

        static void timedOut(WaitSite s) { Fn(s.address); }

        static void finished(WaitSite,
                             std::uint32_t) {}
    };
}   // namespace OnTimeout

// The image's wait policy, a late hook (header comment): type, one of OnTimeout:: or the application's own.
template<typename T, typename... Ts>
struct WaitPolicy {
    using type = OnTimeout::Unbounded;
};

// waitUntil's default PolicyT: the image's WaitPolicy.
struct ImagePolicy {};

template<typename BoundT,
         typename PolicyT = ImagePolicy,
         typename... Dummy,
         typename Cond>
    requires(sizeof...(Dummy) == 0)
[[gnu::always_inline]] inline WaitResult waitUntil(Cond const&   cond,
                                                   BoundT const& bound = BoundT{}) {
    using P = std::conditional_t<
      std::is_same_v<PolicyT, ImagePolicy>,
      typename ::Kvasir::Port::LateT<WaitPolicy<::Kvasir::Tag::User>, Dummy...>::type,
      PolicyT>;
    static_assert(WaitPolicyShape<P>,
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
