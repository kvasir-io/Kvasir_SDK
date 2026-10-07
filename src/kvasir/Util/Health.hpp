#pragma once
// A health-gated watchdog: subsystems declare a check next to their other Startup hooks and check in when they make
// progress; the supervisor feeds the watchdog only while every check is inside its window. A check that falls
// silent is named - in the log, and in a CRC-checked .noInit record the next boot reports.
//
//     struct I2cHealth {
//         static constexpr std::string_view name = "i2c1";
//         static constexpr auto maxInterval      = std::chrono::milliseconds{200};   // silent longer: starved
//     };
//     struct I2cEngine {
//         static void handler();   // if(progressed) Kvasir::Health::checkIn<I2cHealth>();
//         using Extends = brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &handler>,
//                                       Kvasir::Health::Check<I2cHealth>>;
//     };
//
//     using Health = Kvasir::Health::Supervisor<Startup, HW::Watchdog, HW::SystickClock, HealthConfig>;
//     int main() {
//         Health::holdOff();                         // a watchdog the last run left armed waits for service()
//         Kvasir::Boot::logBoot(Kvasir::PM::reset_cause());
//         Health::logReport();                       // names the check that starved last time
//         while(true) { Startup::run<Kvasir::Hook::MainLoop>(); Health::service(); }
//     }
//
// The supervisor owns the watchdog: its first service() arms it (a watchdog with arm(); one without is fed), from
// then on only service() feeds, and holdOff() - before that first turn only - stops one that is already running.
//
// Check Config: name, maxInterval (required), graceAfterBoot (default maxInterval: the first window is grace +
// maxInterval), startSuspended (false; Supervisor::resume<C>() starts it). Supervisor Config: period (10 ms),
// statistics (true: Supervisor::worst() keeps each check's longest silence).
//
// Policies on a starvation, injected (`template<> inline constexpr auto Kvasir::Health::injectedPolicy<> = ...`):
// StopFeeding (the default: log, record, announce the coming reset to a printer, let the watchdog bite), RaisePanic
// (Panic::Cause::healthCheck, the check's index as the detail: the application's panic handler makes it safe),
// LogOnly (bench: log, keep feeding). A supervised image should set its watchdog's `gatedFeed`, so only the
// supervisor can feed, arm or disarm it (Util/HealthKey.hpp).
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/AnnouncedReset.hpp"
#include "kvasir/Util/HealthKey.hpp"
#include "kvasir/Util/Panic.hpp"
#include "kvasir/Util/Periodic.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Health {
template<typename C>
concept CheckConfig = requires {
    { std::string_view{C::name} };
    { std::chrono::duration_cast<std::chrono::milliseconds>(C::maxInterval) };
};

template<CheckConfig C>
struct Check {
    using Hook = Kvasir::Hook::HealthCheck;

    static void noop() {}

    static constexpr auto fn = &noop;
    using config             = C;
    // Written by the checking context only; read by the supervisor, maybe on the other core. An aligned word store
    // is single-copy atomic (Armv6-M DDI0419E A3.5.1, Armv8-M DDI0553B.y B7.2.1): no lock, the supervisor only
    // asks "did it change".
    static inline std::uint32_t volatile beats = 0;
};

/// Progress: one load and one store.
template<CheckConfig C>
[[gnu::always_inline]] inline void checkIn() {
    Check<C>::beats = Check<C>::beats + 1;
}

// The record of a starvation, Persistent "KHLT" v1.
struct Starvation {
    std::uint32_t check;      // index in the supervisor's list
    std::uint32_t nameHash;   // FNV-1a of the name: the next build may order the checks differently
    std::uint32_t silentMs;
    std::uint32_t allowedMs;
    std::uint32_t uptimeMs;
    std::uint32_t silentMask;   // the checks past half their window at that moment
};

struct StarvationTag {
    static constexpr std::uint32_t id      = persistentId("KHLT");
    static constexpr std::uint16_t version = 1;
};

[[KVASIR_SECTION(".noInit")]] inline Persistent<Starvation, StarvationTag> lastStarvation;

/// Whether a starvation record is there, without taking it (a boot guard).
inline bool starvationRecorded() { return lastStarvation.valid(); }

constexpr std::uint32_t fnv1a(std::string_view s) {
    std::uint32_t h = 0x811C'9DC5U;
    for(char const c : s) { h = (h ^ static_cast<std::uint8_t>(c)) * 0x0100'0193U; }
    return h;
}

struct Starved {
    std::uint32_t    check;
    std::string_view name;
    std::uint32_t    silentMs;
    std::uint32_t    allowedMs;
    std::uint32_t    timeoutMs;   // the watchdog's, 0 if unknown
};

struct StopFeeding {
    static constexpr bool keepFeeding = false;

    static void operator()([[maybe_unused]] Starved const& s) {
#ifdef USE_UC_LOG
        UC_LOG_C("health: check '{}' silent {} ms (allowed {} ms): watchdog reset follows",
                 s.name,
                 s.silentMs,
                 s.allowedMs);
#endif
        // a printer must stay off the chip through the reset that comes (AnnouncedReset.hpp)
        (void)AnnouncedReset::announce(s.timeoutMs + 1'000);
    }
};

struct LogOnly {
    static constexpr bool keepFeeding = true;

    static void operator()([[maybe_unused]] Starved const& s) {
#ifdef USE_UC_LOG
        UC_LOG_E("health: check '{}' silent {} ms (allowed {} ms), still feeding (LogOnly)",
                 s.name,
                 s.silentMs,
                 s.allowedMs);
#endif
    }
};

struct RaisePanic {
    static constexpr bool keepFeeding = false;

    [[noreturn]] static void operator()(Starved const& s) {
        Panic::raiseAt(Panic::Cause::healthCheck, 0, s.check);
    }
};

template<typename...>
inline constexpr auto injectedPolicy = StopFeeding{};

struct DefaultConfig {};

namespace Detail {
    template<typename Config,
             typename Clock>
    constexpr auto periodOf() {
        if constexpr(requires { Config::period; }) {
            return std::chrono::duration_cast<typename Clock::duration>(Config::period);
        } else {
            return std::chrono::duration_cast<typename Clock::duration>(
              std::chrono::milliseconds{10});
        }
    }

    template<typename C>
    constexpr auto graceOf() {
        if constexpr(requires { C::graceAfterBoot; }) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(C::graceAfterBoot);
        } else {
            return std::chrono::duration_cast<std::chrono::milliseconds>(C::maxInterval);
        }
    }

    template<typename C>
    constexpr bool startSuspendedOf() {
        if constexpr(requires { C::startSuspended; }) {
            return C::startSuspended;
        } else {
            return false;
        }
    }

    template<typename List>
    struct Configs;

    template<typename... Cs>
    struct Configs<brigand::list<Cs...>> {
        static constexpr std::size_t                                 size = sizeof...(Cs);
        static constexpr std::array<std::string_view, sizeof...(Cs)> names{
          std::string_view{Cs::config::name}...};
        static constexpr std::array<std::uint32_t, sizeof...(Cs)> allowedMs{
          static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Cs::config::maxInterval)
              .count())...};
        static constexpr std::array<std::uint32_t, sizeof...(Cs)> graceMs{
          static_cast<std::uint32_t>(graceOf<typename Cs::config>().count())...};
        static constexpr std::uint32_t initiallySuspended = [] {
            constexpr std::array<bool, sizeof...(Cs)> suspended{
              startSuspendedOf<typename Cs::config>()...};
            std::uint32_t m = 0;
            for(std::size_t i = 0; i < suspended.size(); ++i) {
                if(suspended[i]) { m |= 1U << i; }
            }
            return m;
        }();

        static std::array<std::uint32_t,
                          sizeof...(Cs)>
        beats() {
            return {Cs::beats...};
        }

        template<typename C>
        static constexpr std::size_t indexOf() {
            constexpr std::array<bool, sizeof...(Cs)> is{std::is_same_v<typename Cs::config, C>...};
            for(std::size_t i = 0; i < is.size(); ++i) {
                if(is[i]) { return i; }
            }
            return sizeof...(Cs);
        }

        static constexpr bool namesUnique() {
            for(std::size_t i = 0; i < size; ++i) {
                for(std::size_t j = i + 1; j < size; ++j) {
                    if(names[i] == names[j]) { return false; }
                }
            }
            return true;
        }
    };
}   // namespace Detail

template<typename Startup, typename Watchdog, typename Clock, typename Config = DefaultConfig>
struct Supervisor {
    using Checks                   = typename Startup::template ExtendsOn<Hook::HealthCheck>;
    using Cfg                      = Detail::Configs<Checks>;
    static constexpr std::size_t N = Cfg::size;
    static_assert(N > 0,
                  "Health::Supervisor without a Health::Check in the Startup lists");
    static_assert(N <= 32,
                  "the starvation record keeps a 32-bit mask of the checks");
    static_assert(Cfg::namesUnique(),
                  "two health checks with the same name");

    static constexpr auto          period   = Detail::periodOf<Config, Clock>();
    static constexpr std::uint32_t periodMs = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(period).count());
    static_assert(periodMs > 0,
                  "Health::Supervisor: a period of at least 1 ms");
    // a watchdog without a Timeout member (a driver that does not say) is taken as it is: `||` does not keep the
    // right-hand side from being looked up, so the member is asked for in a discarded branch
    static constexpr bool periodBelowTimeout = [] {
        if constexpr(requires { Watchdog::Timeout; }) {
            return Watchdog::Timeout > 2 * period;
        } else {
            return true;
        }
    }();
    static_assert(periodBelowTimeout,
                  "Health::Supervisor: the period must be well below the watchdog's timeout");
    static constexpr bool statistics = [] {
        if constexpr(requires { Config::statistics; }) {
            return Config::statistics;
        } else {
            return true;
        }
    }();
    static constexpr std::uint32_t timeoutMs = [] {
        if constexpr(requires { Watchdog::Timeout; }) {
            return static_cast<std::uint32_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(Watchdog::Timeout).count());
        } else {
            return 0U;
        }
    }();

    /// Once per main-loop turn (or from an executor every period): judges every check, feeds the watchdog only
    /// while all are in their windows.
    static void service(typename Clock::time_point now = Clock::now()) {
        if(!started_) {
            started_ = true;
            tick_    = Every<Clock>{period, now + period};
            seen_    = Cfg::beats();
            for(std::size_t i = 0; i < N; ++i) {
                limitMs_[i] = Cfg::graceMs[i] + Cfg::allowedMs[i];
            }
            startWatchdog();
            return;
        }
        auto const k = tick_.dueCount(now);
        if(k == 0) { return; }
        auto const                 beats      = Cfg::beats();
        std::uint32_t              silentMask = 0;
        bool                       all        = true;
        std::optional<std::size_t> starved{};
        for(std::size_t i = 0; i < N; ++i) {
            std::uint32_t const bit = 1U << i;
            if(beats[i] != seen_[i]) {
                seen_[i]     = beats[i];
                silentMs_[i] = 0;
                limitMs_[i]  = Cfg::allowedMs[i];   // past the first window
                everBeat_ |= bit;
            } else if((suspended_ & bit) == 0) {
                silentMs_[i] = silentMs_[i] + k * periodMs;
                if constexpr(statistics) { worstMs_[i] = std::max(worstMs_[i], silentMs_[i]); }
                if(silentMs_[i] * 2 > limitMs_[i]) { silentMask |= bit; }
                if(silentMs_[i] > limitMs_[i] && !starved) { starved = i; }
            }
            if((everBeat_ & bit) == 0 && (suspended_ & bit) == 0) { all = false; }
        }
        if(!reported_ && all) {
            reported_ = true;
            Startup::template run<Hook::AllChecksReported>();
        }
        if(starved && !latched_) { starve<>(*starved, silentMask, now); }
        if(!latched_) { feedWatchdog(); }
    }

    /// Whether the watchdog can be stopped at all (an STM32 IWDG cannot): holdOff() is only there if it can.
    static constexpr bool canHoldOff
      = requires(FeedKey key) { Watchdog::disarm(key); } || requires { Watchdog::disarm(); };

    /// Before the first service(), where the boot may take longer than the watchdog allows: stops a watchdog that
    /// is already running - the run before left it armed (the RP2350 keeps it enabled across a watchdog reset), or
    /// the Startup list armed it. The first service() arms it again. Once the supervisor runs this does nothing:
    /// it is no way to switch a supervised watchdog off.
    static void holdOff()
        requires canHoldOff
    {
        if(started_) { return; }
        if constexpr(requires(FeedKey key) { Watchdog::disarm(key); }) {
            Watchdog::disarm(FeedKey{});
        } else {
            Watchdog::disarm();
        }
    }

    template<CheckConfig C>
    static void suspend() {
        suspended_ = suspended_ | bitOf<C>();
    }

    template<CheckConfig C>
    static void resume() {
        suspended_              = suspended_ & ~bitOf<C>();
        silentMs_[indexOf<C>()] = 0;
    }

    /// The longest silence of check i so far, in ms (statistics).
    [[nodiscard]] static std::uint32_t worstMs(std::size_t i) { return worstMs_[i]; }

    [[nodiscard]] static std::string_view name(std::size_t i) { return Cfg::names[i]; }

    /// The previous run's starvation, once.
    static std::optional<Starvation> takeLastStarvation() { return lastStarvation.take(); }

    /// One line if the run before this one starved a check (takes the record).
    static void logReport() {
        [[maybe_unused]] auto const s = takeLastStarvation();
        if(!s) { return; }
#ifdef USE_UC_LOG
        bool const known = s->check < N && fnv1a(Cfg::names[s->check]) == s->nameHash;
        UC_LOG_E(
          "health: the run before this one starved check '{}' (#{}): silent {} ms, allowed {} ms, "
          "{} ms after "
          "boot; checks past half their window {:#x}",
          known ? Cfg::names[s->check] : std::string_view{"(not in this build)"},
          s->check,
          s->silentMs,
          s->allowedMs,
          s->uptimeMs,
          s->silentMask);
#endif
    }

private:
    template<CheckConfig C>
    static constexpr std::size_t indexOf() {
        return Cfg::template indexOf<C>();
    }

    template<CheckConfig C>
    static constexpr std::uint32_t bitOf() {
        constexpr auto i = indexOf<C>();
        static_assert(i < N, "this check is not in the supervisor's Startup lists");
        return 1U << i;
    }

    static void feedWatchdog() {
        if constexpr(requires(FeedKey key) { Watchdog::feed(key); }) {
            Watchdog::feed(FeedKey{});
        } else {
            Watchdog::feed();
        }
    }

    // the first turn: arm (which loads the full timeout), or feed a watchdog that has no arm()
    static void startWatchdog() {
        if constexpr(requires(FeedKey key) { Watchdog::arm(key); }) {
            Watchdog::arm(FeedKey{});
        } else if constexpr(requires { Watchdog::arm(); }) {
            Watchdog::arm();
        } else {
            feedWatchdog();
        }
    }

    template<typename... Dummy>
        requires(sizeof...(Dummy) == 0)
    static void starve(std::size_t                i,
                       std::uint32_t              silentMask,
                       typename Clock::time_point now) {
        auto const& policy = injectedPolicy<Dummy...>;
        latched_           = !std::remove_cvref_t<decltype(policy)>::keepFeeding;
        auto const uptime  = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        lastStarvation.store(Starvation{.check      = static_cast<std::uint32_t>(i),
                                        .nameHash   = fnv1a(Cfg::names[i]),
                                        .silentMs   = silentMs_[i],
                                        .allowedMs  = limitMs_[i],
                                        .uptimeMs   = uptime,
                                        .silentMask = silentMask});
        policy(Starved{.check     = static_cast<std::uint32_t>(i),
                       .name      = Cfg::names[i],
                       .silentMs  = silentMs_[i],
                       .allowedMs = limitMs_[i],
                       .timeoutMs = timeoutMs});
        if(!latched_) { silentMs_[i] = 0; }   // LogOnly: a fresh window
    }

    static inline bool started_{};
    // not read from the clock at static initialisation (it may not run yet): rebased at the first service()
    static inline Every<Clock>                 tick_{period, typename Clock::time_point{}};
    static inline std::array<std::uint32_t, N> seen_{};
    static inline std::array<std::uint32_t, N> silentMs_{};
    static inline std::array<std::uint32_t, N> limitMs_{};
    static inline std::array<std::uint32_t, N> worstMs_{};
    static inline std::uint32_t                suspended_ = Cfg::initiallySuspended;
    static inline std::uint32_t                everBeat_{};
    static inline bool                         latched_{};
    static inline bool                         reported_{};
};
}   // namespace Kvasir::Health
