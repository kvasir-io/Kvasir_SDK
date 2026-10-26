#pragma once
// The health supervisor (kvasir/Health/Health.hpp is the umbrella and the user guide): the check table, the turn that
// judges every check and feeds the watchdog, suspend/resume, the starvation path and the boot report. The ladder's
// helpers and state are in Health/Ladder.hpp, the policies in Health/Policies.hpp.
#include "kvasir/Atomic/CriticalSection.hpp"
#include "kvasir/Health/Check.hpp"
#include "kvasir/Health/HealthKey.hpp"
#include "kvasir/Health/HealthRecords.hpp"
#include "kvasir/Health/Ladder.hpp"
#include "kvasir/Health/Policies.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Util/Config.hpp"
#include "kvasir/Util/Periodic.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
struct DefaultConfig {};

namespace Detail {
    template<typename Config,
             typename Clock>
    constexpr auto periodOf() {
        return std::chrono::duration_cast<typename Clock::duration>(
          KVASIR_CONFIG_OR(Config, period, std::chrono::milliseconds{10}));
    }

    template<typename C>
    constexpr auto graceOf() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
          KVASIR_CONFIG_OR(C, graceAfterBoot, C::maxInterval));
    }

    template<typename C>
    constexpr bool startSuspendedOf() {
        return KVASIR_CONFIG_OR(C, startSuspended, false);
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
        static constexpr std::array<std::uint8_t, sizeof...(Cs)> attempts{
          attemptsOf<typename Cs::config>()...};
        static constexpr std::array<std::uint32_t, sizeof...(Cs)> recoveryWindowMs{
          static_cast<std::uint32_t>(recoveryWindowOf<typename Cs::config>().count())...};
        static constexpr bool anyRecovery = (Recovers<typename Cs::config> || ...);
        // only instantiated where anyRecovery
        static constexpr std::array<Recovery (*)(Attempt), sizeof...(Cs)> onStarved{
          onStarvedOf<typename Cs::config>()...};
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
    static_assert(WatchdogShape<Watchdog>,
                  "Health::Supervisor: the watchdog needs feed() (or feed(FeedKey) when gated) and "
                  "a Timeout duration "
                  "(Health/HealthKey.hpp WatchdogShape)");
    using Checks                   = typename Startup::template ExtendsOn<Hook::HealthCheck>;
    using Cfg                      = Detail::Configs<Checks>;
    using ClockType                = Clock;
    static constexpr std::size_t n = Cfg::size;
    static_assert(n > 0,
                  "Health::Supervisor without a Health::Check in the Startup lists");
    static_assert(n <= 32,
                  "the starvation record keeps a 32-bit mask of the checks");
    static_assert(Cfg::namesUnique(),
                  "two health checks with the same name");

    static constexpr auto          period   = Detail::periodOf<Config, Clock>();
    static constexpr std::uint32_t periodMs = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(period).count());
    static_assert(periodMs > 0,
                  "Health::Supervisor: a period of at least 1 ms");
    static constexpr bool periodBelowTimeout = Watchdog::timeout > 2 * period;
    static_assert(periodBelowTimeout,
                  "Health::Supervisor: the period must be well below the watchdog's timeout");
    static constexpr bool          statistics = KVASIR_CONFIG_OR(Config, statistics, true);
    static constexpr std::uint32_t timeoutMs  = static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Watchdog::timeout).count());

    static constexpr auto recoveryBudget = std::chrono::duration_cast<typename Clock::duration>(
      KVASIR_CONFIG_OR(Config, recoveryBudget, std::chrono::milliseconds{2}));
    /// Recoveries run by the main loop (runRecoveries()), not by service() (header comment).
    static constexpr bool deferRecoveries
      = static_cast<bool>(KVASIR_CONFIG_OR(Config, deferRecoveries, false));

    // what the watchdog sees between two feeds while a check recovers: two periods and one onStarved call
    static constexpr bool budgetBelowTimeout = [] {
        if constexpr(!Cfg::anyRecovery || deferRecoveries) {
            return true;
        } else {
            return Watchdog::timeout > 2 * period + recoveryBudget;
        }
    }();
    static_assert(budgetBelowTimeout,
                  "Health: period x 2 + recoveryBudget must stay below the watchdog's timeout");

    /// From check C's last beat to the reset under StopFeeding, at most (ms): its window, every rung's window, one
    /// period per judgement, the watchdog's timeout.
    template<CheckConfig C>
    static constexpr std::uint32_t worstToEscalationMs() {
        constexpr auto i = indexOf<C>();
        static_assert(i < n, "this check is not in the supervisor's Startup lists");
        return Cfg::allowedMs[i] + Cfg::attempts[i] * Cfg::recoveryWindowMs[i]
             + (Cfg::attempts[i] + 1U) * periodMs + timeoutMs;
    }

    /// Once per main-loop turn (or from an executor every period): judges every check, feeds the watchdog only
    /// while all are in their windows.
    static void service(typename Clock::time_point now = Clock::now()) {
        if(!started_) {
            started_ = true;
            tick_    = Every<Clock>{period, now + period};
            seen_    = Cfg::beats();
            for(std::size_t i = 0; i < n; ++i) {
                limitMs_[i] = Cfg::graceMs[i] + Cfg::allowedMs[i];
            }
            // this run's recoveries start at none (logReport() took the last run's before)
            if constexpr(Cfg::anyRecovery) { Ladder::storeRecord(); }
            startWatchdog();
            return;
        }
        auto const k = tick_.dueCount(now);
        if(k == 0) { return; }
        auto const                 beats      = Cfg::beats();
        std::uint32_t              silentMask = 0;
        bool                       all        = true;
        std::optional<std::size_t> starved{};
        if constexpr(Cfg::anyRecovery) { Ladder::beginTurn(); }
        for(std::size_t i = 0; i < n; ++i) {
            std::uint32_t const bit = 1U << i;
            if(beats[i] != seen_[i]) {
                seen_[i]     = beats[i];
                silentMs_[i] = 0;
                limitMs_[i]  = Cfg::allowedMs[i];   // past the first window
                everBeat_ |= bit;
                if constexpr(Cfg::anyRecovery) { Ladder::onBeat(i, bit, now); }
            } else if((suspended_ & bit) == 0) {
                silentMs_[i] = silentMs_[i] + k * periodMs;
                if constexpr(statistics) { worstMs_[i] = std::max(worstMs_[i], silentMs_[i]); }
                if(silentMs_[i] * 2 > limitMs_[i]) { silentMask |= bit; }
                if constexpr(!Cfg::anyRecovery) {
                    if(silentMs_[i] > limitMs_[i] && !starved) { starved = i; }
                } else if(!starved && Ladder::takeRefused(bit)) {
                    // deferRecoveries: its onStarved returned cannot - escalate, with the silence of the request
                    starved = i;
                    Ladder::refusedSilence(i);
                } else if(silentMs_[i] > limitMs_[i]) {
                    if(!Ladder::rescue(i) && !starved) { starved = i; }
                }
            }
            if((everBeat_ & bit) == 0 && (suspended_ & bit) == 0) { all = false; }
        }
        if(!reported_ && all) {
            reported_ = true;
            Startup::template run<Hook::AllChecksReported>();
        }
        if(starved && !latched_) {
            if constexpr(Cfg::anyRecovery) { Ladder::escalating(*starved); }
            starve<>(*starved, silentMask, now);
        }
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
        if constexpr(Cfg::anyRecovery) {
            Ladder::onResume(indexOf<C>(), bitOf<C>());
        }   // a fresh ladder
    }

    /// The longest silence of check i so far, in ms (statistics).
    [[nodiscard]] static std::uint32_t worstMs(std::size_t i) { return worstMs_[i]; }

    [[nodiscard]] static std::string_view name(std::size_t i) { return Cfg::names[i]; }

    /// deferRecoveries: runs one recovery service() requested, in thread mode, from the main loop - every turn, the
    /// same core as service(). Returns true if it ran one (always false in an image without an onStarved).
    static bool runRecoveries()
        requires deferRecoveries
    {
        if constexpr(!Cfg::anyRecovery) {
            return false;
        } else {
            return Ladder::runRecoveries();
        }
    }

    /// deferRecoveries: mask of the checks with a recovery requested and not yet run.
    [[nodiscard]] static std::uint32_t pendingRecoveries()
        requires deferRecoveries
    {
        if constexpr(!Cfg::anyRecovery) {
            return 0;
        } else {
            return Ladder::pendingRecoveries();
        }
    }

    /// The previous run's starvation, once.
    static std::optional<Starvation> takeLastStarvation() { return lastStarvation.take(); }

    /// The previous run's recoveries, once (only an image with an onStarved writes them).
    static std::optional<Recoveries> takeLastRecoveries() { return lastRecovery.take(); }

    /// Recoveries of check i that ended in a beat, this run.
    [[nodiscard]] static std::uint32_t recoveredCount(std::size_t i)
        requires(Cfg::anyRecovery)
    {
        return rec_.recovered[i];
    }

    /// Rungs check i has used since its last beat.
    [[nodiscard]] static std::uint32_t attemptOf(std::size_t i)
        requires(Cfg::anyRecovery)
    {
        return rec_.attempt[i];
    }

    /// onStarved calls over recoveryBudget, this run.
    [[nodiscard]] static std::uint32_t recoveryOverruns()
        requires(Cfg::anyRecovery)
    {
        return rec_.overruns;
    }

    /// One line if the run before this one starved a check (takes the record); with an onStarved in the image, one
    /// line each for its recoveries, a reset inside onStarved and an escalation after recoveries. Call it before the
    /// first service(), which starts this run's recovery record afresh.
    static void logReport() {
        if constexpr(Cfg::anyRecovery) { Ladder::logRecoveries(); }
        [[maybe_unused]] auto const s = takeLastStarvation();
        if(!s) { return; }
#ifdef USE_UC_LOG
        bool const known = s->check < n && fnv1a(Cfg::names[s->check]) == s->nameHash;
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

    /// The main loop's step (Port/Extends.hpp verbs), for Health::Service<> or a hand call: service(), and when the
    /// loop is next needed - the next tick. With deferRecoveries service() is the interrupt's: turn() runs the
    /// recoveries it requested instead (one per call, as runRecoveries()), busy while more are pending.
    static Kvasir::Turn turn() {
        if constexpr(deferRecoveries) {
            (void)runRecoveries();
            return pendingRecoveries() != 0 ? Kvasir::Turn::busy() : Kvasir::Turn::idle();
        } else {
            auto const now = Clock::now();
            service(now);
            return Kvasir::Turn::within(tick_.next() - now);
        }
    }

private:
    // the recovery ladder (Health/Ladder.hpp): its functions, on this supervisor's state
    using Ladder = Detail::Ladder<Supervisor>;
    friend Ladder;

    template<CheckConfig C>
    static constexpr std::size_t indexOf() {
        return Cfg::template indexOf<C>();
    }

    template<CheckConfig C>
    static constexpr std::uint32_t bitOf() {
        constexpr auto i = indexOf<C>();
        static_assert(i < n, "this check is not in the supervisor's Startup lists");
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
        using Reaction =
          typename ::Kvasir::Port::LateT<StarvationReaction<::Kvasir::Tag::User>, Dummy...>::type;
        latched_ = !Reaction::keepFeeding;
        // no recovery runs while the reset (or the panic) comes
        if constexpr(Cfg::anyRecovery && deferRecoveries) { Ladder::onLatched(); }
        auto const uptime = uptimeMsOf(now);
        lastStarvation.store(Starvation{.check      = static_cast<std::uint32_t>(i),
                                        .nameHash   = fnv1a(Cfg::names[i]),
                                        .silentMs   = silentMs_[i],
                                        .allowedMs  = limitMs_[i],
                                        .uptimeMs   = uptime,
                                        .silentMask = silentMask});
        Reaction{}(Starved{.check     = static_cast<std::uint32_t>(i),
                           .name      = Cfg::names[i],
                           .silentMs  = silentMs_[i],
                           .allowedMs = limitMs_[i],
                           .timeoutMs = timeoutMs});
        if(!latched_) {   // a reaction that keeps feeding (OnStarvation::Log): a fresh window
            silentMs_[i] = 0;
            if constexpr(Cfg::anyRecovery) {
                Ladder::onFreshWindow(i);
            }   // and a fresh ladder for the next silence
        }
    }

    [[nodiscard]] static std::uint32_t uptimeMsOf(typename Clock::time_point now) {
        return static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    static inline bool started_{};
    // not read from the clock at static initialisation (it may not run yet): rebased at the first service()
    static inline Every<Clock>                 tick_{period, typename Clock::time_point{}};
    static inline std::array<std::uint32_t, n> seen_{};
    static inline std::array<std::uint32_t, n> silentMs_{};
    static inline std::array<std::uint32_t, n> limitMs_{};
    static inline std::array<std::uint32_t, n> worstMs_{};
    static inline std::uint32_t                suspended_ = Cfg::initiallySuspended;
    static inline std::uint32_t                everBeat_{};
    static inline bool                         latched_{};
    static inline bool                         reported_{};
    static inline Detail::RecoveryState<n, Cfg::anyRecovery>                 rec_{};
    static inline Detail::DeferState<n, Cfg::anyRecovery && deferRecoveries> defer_{};
};

// The supervisor as a Startup entry: `Kvasir::Health::Service<>` in the list drives Supervisor::turn() from
// Hook::MainLoop - no hand service() in the loop, and a sleeping loop (Startup::runTurn) sees the next tick. A
// supervisor names its Startup (it reads the checks from the lists), so the list cannot name the supervisor: the
// firmware binds it in the late-hook form (kvasir/Port/Hooks.hpp) right after the supervisor's alias, before anything
// else instantiates the Startup:
//
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, ..., Kvasir::Health::Service<>>;
//     using Health  = Kvasir::Health::Supervisor<Startup, HW::Watchdog, HW::SystickClock, HealthConfig>;
//     template<>
//     struct Kvasir::Health::SupervisorFor<Kvasir::Tag::User> { using type = ::Health; };
//
// `::Health`, not `Health`: inside the specialisation the name is looked up in Kvasir::Health first, and finds that
// namespace.
// List it after the entries whose checks it judges (they beat in the same turn), after the boot report (the list rule
// HealthServiceAfterReport) and after the chip watchdog (WatchdogBeforeHealthService: one that its own runtimeInit arms
// after the hold-off would count through the boot; a gated one has no runtimeInit and may come after), and do not also
// call service() or runRecoveries() by hand: it would run twice a turn. Its runtimeInit is the supervisor's boot half,
// before main: holdOff() where the watchdog can be stopped, then logReport() - so main calls neither. A supervisor
// without Service<> (service() by hand) keeps both first in main. With Boot::ReportInMain (main reads the records
// itself) the rule has no report entry to order against: the service's starvation line then comes before main's boot
// line.
template<typename T, typename... Ts>
struct SupervisorFor {
    static_assert(Kvasir::MPL::AlwaysFalse<T>::value,
                  "Health::Service<> without `template<> struct "
                  "Kvasir::Health::SupervisorFor<Kvasir::Tag::User> { "
                  "using type = <the Supervisor>; };` right after the supervisor's alias (or the "
                  "binding comes after "
                  "a use of the Startup)");
};

template<typename Tag = ::Kvasir::Tag::User>
struct Service {
    static Kvasir::Turn turn() {
        return ::Kvasir::Port::LateT<SupervisorFor<Tag>, Tag>::type::turn();
    }

    // a watchdog still counting from the run before waits for the first turn; then the run before's starvation
    static void runtimeInit() {
        using S = typename ::Kvasir::Port::LateT<SupervisorFor<Tag>, Tag>::type;
        if constexpr(S::canHoldOff) { S::holdOff(); }
        S::logReport();
    }

    // for the list rule HealthServiceAfterReport (StartUp/ListRules.hpp): runtimeInit's line follows the boot line
    static constexpr bool isHealthService = true;

    using Extends = Startup::Extend<Hook::MainLoop, &Service::turn>;
};
}   // namespace Kvasir::Health
