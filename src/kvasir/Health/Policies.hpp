#pragma once
// The starvation reactions of the health supervisor (kvasir/Health/Health.hpp): what happens to a check that stays
// silent - OnStarvation::StopFeeding (the default), Log, Panic - and StarvationReaction, the late hook (the table in
// kvasir/Port/Hooks.hpp) an application specialises after including the umbrella:
//     template<>
//     struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
//         using type = Kvasir::Health::OnStarvation::Log;
//     };
#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Runtime/AnnouncedReset.hpp"
#include "kvasir/Runtime/Panic.hpp"

#include <cstdint>
#include <string_view>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Health {
struct Starved {
    std::uint32_t    check;
    std::string_view name;
    std::uint32_t    silentMs;
    std::uint32_t    allowedMs;
    std::uint32_t    timeoutMs;   // the watchdog's Timeout
};

namespace OnStarvation {
    struct StopFeeding {
        static constexpr bool keepFeeding = false;

        static void operator()([[maybe_unused]] Starved const& s) {
#ifdef USE_UC_LOG
            ::Kvasir::Panic::
              beginFatal();   // fatal end: the watchdog, no longer fed, resets the chip
            UC_LOG_C("health: check '{}' silent {} ms (allowed {} ms): watchdog reset follows",
                     s.name,
                     s.silentMs,
                     s.allowedMs);
#endif
            // a printer must stay off the chip through the reset that comes (AnnouncedReset.hpp)
            (void)AnnouncedReset::announce(s.timeoutMs + 1'000);
        }
    };

    struct Log {
        static constexpr bool keepFeeding = true;

        static void operator()([[maybe_unused]] Starved const& s) {
#ifdef USE_UC_LOG
            UC_LOG_E(
              "health: check '{}' silent {} ms (allowed {} ms), still feeding (OnStarvation::Log)",
              s.name,
              s.silentMs,
              s.allowedMs);
#endif
        }
    };

    struct Panic {
        static constexpr bool keepFeeding = false;

        [[noreturn]] static void operator()(Starved const& s) {
            ::Kvasir::Panic::raiseAt(::Kvasir::Panic::Cause::healthCheck, 0, s.check);
        }
    };

    // Count it, no line, keep feeding (a fresh window, as Log): `starvations`.
    struct Count {
        static constexpr bool keepFeeding = true;
        static inline std::uint32_t volatile starvations{};

        static void operator()(Starved const&) { starvations = starvations + 1; }
    };

    // The application's function, `void fn(Starved const&)`, in the supervisor's context (thread mode, or the
    // interrupt that services it), instead of StopFeeding's line. KeepFeeding false (the default): the watchdog resets
    // afterwards as under StopFeeding - fn announces it to a printer if it wants one off the chip
    // (AnnouncedReset::announce(s.timeoutMs + 1'000), as StopFeeding does); true: a fresh window, as Log.
    template<auto Fn, bool KeepFeeding = false>
    struct Call {
        static constexpr bool keepFeeding = KeepFeeding;

        static void operator()(Starved const& s) { Fn(s); }
    };

    // Nothing, keep feeding: the check gets a fresh window, the starvation record (lastStarvation) is still written.
    struct Ignore {
        static constexpr bool keepFeeding = true;

        static void operator()(Starved const&) {}
    };

    // "In a row" for a supervisor: the starvations of this run (any check; a fresh window after each, as Log). Each
    // one before the Limit-th is Log's error line and the watchdog stays fed; the Limit-th is Panic
    // (Panic::Cause::healthCheck, the check's index as the detail), after its crit line.
    template<std::uint32_t Limit>
    struct PanicAfter {
        static_assert(Limit >= 1,
                      "PanicAfter<0> would never let a check starve: use OnStarvation::Panic");
        static constexpr bool          keepFeeding = true;
        static constexpr std::uint32_t limit       = Limit;
        static inline std::uint32_t volatile starvations{};

        static void operator()(Starved const& s) {
            starvations = starvations + 1;
            if(starvations >= Limit) {
#ifdef USE_UC_LOG
                KVASIR_DETAIL_FATAL_AT(
                  ::Kvasir::Panic::Cause::healthCheck,
                  0,
                  s.check,
                  "health: check '{}' silent {} ms (allowed {} ms): starvation {} of {}, panic",
                  s.name,
                  s.silentMs,
                  s.allowedMs,
                  static_cast<std::uint32_t>(starvations),
                  Limit);
#else
                KVASIR_DETAIL_FATAL_AT(::Kvasir::Panic::Cause::healthCheck, 0, s.check);
#endif
            }
#ifdef USE_UC_LOG
            UC_LOG_E(
              "health: check '{}' silent {} ms (allowed {} ms): starvation {} of {}, still feeding",
              s.name,
              s.silentMs,
              s.allowedMs,
              static_cast<std::uint32_t>(starvations),
              Limit);
#endif
        }
    };
}   // namespace OnStarvation

// The image's reaction (header comment): type, one of OnStarvation:: or the application's own (a struct with
// `static constexpr bool keepFeeding` and `static void operator()(Starved const&)`).
template<typename T, typename... Ts>
struct StarvationReaction {
    using type = OnStarvation::StopFeeding;
};
}   // namespace Kvasir::Health
