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
//
// A local recovery before the policy: a check Config may have `onStarved`, in one of four forms - `void()`,
// `Recovery()`, `void(Attempt)`, `Recovery(Attempt)` (void counts as Recovery::started). When the check starves, the
// supervisor calls it instead of the policy, gives the check `recoveryWindow` (default maxInterval) to beat again
// and goes on feeding the watchdog meanwhile; it tries `recoveryAttempts` times (default 1, at most 15), a beat
// starts the ladder over, and only a check still silent after the last rung (or one whose onStarved returns
// Recovery::cannot) goes to the policy. Each recovery is logged and kept in the "KHRV" record, which logReport()
// reports at the next boot - also a reset that happened inside onStarved.
//
//     struct BusHealth {
//         static constexpr std::string_view name = "i2c";
//         static constexpr auto maxInterval      = std::chrono::seconds{10};
//         static constexpr auto recoveryAttempts = 2;
//         static constexpr auto recoveryWindow   = std::chrono::seconds{3};
//         static Kvasir::Health::Recovery onStarved(Kvasir::Health::Attempt a);   // a.n = 0 .. a.of - 1
//     };
//
// The contract of onStarved: it runs in thread mode, from service() (on core 0 for a core 1 check too). It STARTS a
// recovery and returns - a request, a flag the owning loop serves, a peripheral reset with a bounded wait - and never
// waits for the subsystem to come back. It does not reset the chip: return Recovery::cannot to escalate, so the
// record and the printer's announcement happen. The time it takes is measured against the Supervisor Config's
// `recoveryBudget` (2 ms): over it, a warning and a count (Supervisor::recoveryOverruns()). A hang inside it is the
// watchdog's, and the next boot names the check and the rung.
//
// The watchdog bounds the main loop, onStarved included: period x 2 + recoveryBudget must stay below its timeout
// (a static_assert). From a check's last beat to the reset under StopFeeding it is at most
// maxInterval + recoveryAttempts x recoveryWindow + (recoveryAttempts + 1) x period + the watchdog's timeout:
// Supervisor::worstToEscalationMs<C>(). Without any onStarved in the image nothing of this is compiled in.
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/AnnouncedReset.hpp"
#ifdef __arm__
    #include "kvasir/Util/Check.hpp"
#endif
#include "kvasir/Util/HealthKey.hpp"
#include "kvasir/Util/HealthRecords.hpp"
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

// A check's local recovery (onStarved), see the header comment.
enum class Recovery : std::uint8_t {
    started,   // a recovery is under way: the check gets a fresh window to beat again
    cannot,    // no lever left: escalate to the policy now
};

struct Attempt {
    std::uint8_t n;    // 0 .. of - 1: the rung of the ladder
    std::uint8_t of;   // the check's recoveryAttempts
};

template<typename C>
concept PlainRecovery = requires { C::onStarved(); };
template<typename C>
concept LadderRecovery = requires(Attempt a) { C::onStarved(a); };
template<typename C>
concept Recovers = PlainRecovery<C> || LadderRecovery<C>;

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
        ::Kvasir::Panic::beginFatal();
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

    template<typename C>
    constexpr std::uint8_t attemptsOf() {
        if constexpr(!Recovers<C>) {
            return 0;
        } else if constexpr(requires { C::recoveryAttempts; }) {
            static_assert(C::recoveryAttempts >= 1 && C::recoveryAttempts <= 15,
                          "Health: recoveryAttempts of a check with onStarved is 1 .. 15");
            return static_cast<std::uint8_t>(C::recoveryAttempts);
        } else {
            return 1;
        }
    }

    template<typename C>
    constexpr std::chrono::milliseconds recoveryWindowOf() {
        if constexpr(requires { C::recoveryWindow; }) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(C::recoveryWindow);
        } else {
            return std::chrono::duration_cast<std::chrono::milliseconds>(C::maxInterval);
        }
    }

#ifdef __arm__
    // the exception number in IPSR: 0 in thread mode (DDI0419E B1.4.2, DDI0553B.y B3.4)
    inline std::uint32_t exceptionNumber() {
        std::uint32_t ipsr;
        asm volatile("mrs %0, ipsr" : "=r"(ipsr));
        return ipsr & 0x1FFU;
    }
#endif

    // a void onStarved counts as `started`
    template<typename C>
    Recovery callOnStarved(Attempt a) {
        // the contract (header comment): thread mode, from service() - never from an interrupt or a fault handler,
        // where a recovery that waits or logs would block the core. A debug check (debug and sanitize variants).
#ifdef __arm__
        KVASIR_DCHECK_EQ(exceptionNumber(),
                         0U,
                         "Health: onStarved runs in thread mode, from service()");
#endif
        auto const call = [a] {
            if constexpr(LadderRecovery<C>) {
                return C::onStarved(a);
            } else {
                static_cast<void>(a);
                return C::onStarved();
            }
        };
        if constexpr(std::is_void_v<decltype(call())>) {
            call();
            return Recovery::started;
        } else {
            static_assert(std::is_same_v<decltype(call()), Recovery>,
                          "Health: onStarved returns void or Kvasir::Health::Recovery");
            return call();
        }
    }

    template<typename C>
    constexpr Recovery (*onStarvedOf())(Attempt) {
        if constexpr(Recovers<C>) {
            return &callOnStarved<C>;
        } else {
            return nullptr;
        }
    }

    // the supervisor's state for the ladder; nothing (never odr-used) in an image without onStarved
    template<std::size_t N, bool On>
    struct RecoveryState {};

    template<std::size_t N>
    struct RecoveryState<N, true> {
        // rungs used since the last beat
        std::array<std::uint8_t, N> attempt{};
        // recoveries that ended in a beat, this run (saturating)
        std::array<std::uint16_t, N> recovered{};
        // mask: in a recovery window now
        std::uint32_t recovering{};
        // onStarved calls over the budget
        std::uint32_t overruns{};
        // one onStarved per service() turn: a turn grows by one recoveryBudget at most
        bool calledThisTurn{};
        // this run's "KHRV", stored whole after each change
        Recoveries record{};
    };

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

    static constexpr auto recoveryBudget = [] {
        if constexpr(requires { Config::recoveryBudget; }) {
            return std::chrono::duration_cast<typename Clock::duration>(Config::recoveryBudget);
        } else {
            return std::chrono::duration_cast<typename Clock::duration>(
              std::chrono::milliseconds{2});
        }
    }();
    // what the watchdog sees between two feeds while a check recovers: two periods and one onStarved call
    static constexpr bool budgetBelowTimeout = [] {
        if constexpr(!Cfg::anyRecovery) {
            return true;
        } else if constexpr(requires { Watchdog::Timeout; }) {
            return Watchdog::Timeout > 2 * period + recoveryBudget;
        } else {
            return true;
        }
    }();
    static_assert(budgetBelowTimeout,
                  "Health: period x 2 + recoveryBudget must stay below the watchdog's timeout");

    /// From check C's last beat to the reset under StopFeeding, at most (ms): its window, every rung's window, one
    /// period per judgement, the watchdog's timeout.
    template<CheckConfig C>
    static constexpr std::uint32_t worstToEscalationMs() {
        constexpr auto i = indexOf<C>();
        static_assert(i < N, "this check is not in the supervisor's Startup lists");
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
            for(std::size_t i = 0; i < N; ++i) {
                limitMs_[i] = Cfg::graceMs[i] + Cfg::allowedMs[i];
            }
            // this run's recoveries start at none (logReport() took the last run's before)
            if constexpr(Cfg::anyRecovery) { storeRecord(); }
            startWatchdog();
            return;
        }
        auto const k = tick_.dueCount(now);
        if(k == 0) { return; }
        auto const                 beats      = Cfg::beats();
        std::uint32_t              silentMask = 0;
        bool                       all        = true;
        std::optional<std::size_t> starved{};
        if constexpr(Cfg::anyRecovery) { rec_.calledThisTurn = false; }
        for(std::size_t i = 0; i < N; ++i) {
            std::uint32_t const bit = 1U << i;
            if(beats[i] != seen_[i]) {
                seen_[i]     = beats[i];
                silentMs_[i] = 0;
                limitMs_[i]  = Cfg::allowedMs[i];   // past the first window
                everBeat_ |= bit;
                if constexpr(Cfg::anyRecovery) {
                    if((rec_.recovering & bit) != 0) { recovered(i, now); }
                    rec_.attempt[i] = 0;   // progress: the ladder starts over
                }
            } else if((suspended_ & bit) == 0) {
                silentMs_[i] = silentMs_[i] + k * periodMs;
                if constexpr(statistics) { worstMs_[i] = std::max(worstMs_[i], silentMs_[i]); }
                if(silentMs_[i] * 2 > limitMs_[i]) { silentMask |= bit; }
                if constexpr(!Cfg::anyRecovery) {
                    if(silentMs_[i] > limitMs_[i] && !starved) { starved = i; }
                } else if(silentMs_[i] > limitMs_[i]) {
                    bool rescued = false;
                    if(rec_.attempt[i] < Cfg::attempts[i] && !latched_) {
                        if(rec_.calledThisTurn) {
                            rescued = true;   // its rung comes next turn
                        } else {
                            rec_.calledThisTurn = true;
                            rescued             = recover(i);
                        }
                    }
                    if(!rescued && !starved) { starved = i; }
                }
            }
            if((everBeat_ & bit) == 0 && (suspended_ & bit) == 0) { all = false; }
        }
        if(!reported_ && all) {
            reported_ = true;
            Startup::template run<Hook::AllChecksReported>();
        }
        if(starved && !latched_) {
            if constexpr(Cfg::anyRecovery) { escalating(*starved); }
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
        if constexpr(Cfg::anyRecovery) {   // a fresh ladder
            constexpr auto i = indexOf<C>();
            if((rec_.recovering & bitOf<C>()) != 0) { limitMs_[i] = Cfg::allowedMs[i]; }
            rec_.recovering = rec_.recovering & ~bitOf<C>();
            rec_.attempt[i] = 0;
        }
    }

    /// The longest silence of check i so far, in ms (statistics).
    [[nodiscard]] static std::uint32_t worstMs(std::size_t i) { return worstMs_[i]; }

    [[nodiscard]] static std::string_view name(std::size_t i) { return Cfg::names[i]; }

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
        if constexpr(Cfg::anyRecovery) { logRecoveries(); }
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
        if(!latched_) {   // LogOnly: a fresh window
            silentMs_[i] = 0;
            if constexpr(Cfg::anyRecovery) {   // and a fresh ladder for the next silence
                limitMs_[i]     = Cfg::allowedMs[i];
                rec_.recovering = rec_.recovering & ~(1U << i);
                rec_.attempt[i] = 0;
            }
        }
    }

    [[nodiscard]] static std::uint32_t uptimeMsOf(typename Clock::time_point now) {
        return static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    // one store for every change (a single CRC loop in the image); the RAM copy is the truth
    [[gnu::noinline]] static void storeRecord() { lastRecovery.store(rec_.record); }

    // check i is past its window with a rung left: call its onStarved; true if it is recovering now
    static bool recover(std::size_t i) {
        std::uint32_t const bit = 1U << i;
        auto const          n   = rec_.attempt[i];
        auto const          of  = Cfg::attempts[i];
        rec_.attempt[i]         = static_cast<std::uint8_t>(n + 1);
        // before the call: a reset inside it is named at the next boot
        rec_.record.inProgress = recoveryInProgress(static_cast<std::uint32_t>(i), n + 1U);
        storeRecord();
#ifdef USE_UC_LOG
        UC_LOG_W("health: check '{}' silent {} ms (allowed {} ms): recovery {} of {}",
                 Cfg::names[i],
                 silentMs_[i],
                 limitMs_[i],
                 n + 1U,
                 of);
#endif
        auto const     t0 = Clock::now();
        Recovery const r  = Cfg::onStarved[i](Attempt{.n = n, .of = of});
        auto const     dt = Clock::now() - t0;
        if(dt > recoveryBudget) {
            rec_.overruns = rec_.overruns + 1;
#ifdef USE_UC_LOG
            UC_LOG_W(
              "health: onStarved of '{}' took {} us (budget {} us)",
              Cfg::names[i],
              static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(dt).count()),
              static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(recoveryBudget).count()));
#endif
        }
        rec_.record.inProgress = 0;
        storeRecord();
        if(r == Recovery::cannot) {
            rec_.attempt[i] = of;   // the ladder is spent: escalate in this turn
#ifdef USE_UC_LOG
            UC_LOG_W("health: onStarved of '{}' cannot recover", Cfg::names[i]);
#endif
            return false;
        }
        rec_.recovering = rec_.recovering | bit;
        silentMs_[i]    = 0;
        limitMs_[i]     = Cfg::recoveryWindowMs[i];
        return true;
    }

    // check i beat inside its recovery window
    static void recovered(std::size_t                i,
                          typename Clock::time_point now) {
        rec_.recovering                   = rec_.recovering & ~(1U << i);
        [[maybe_unused]] auto const rungs = rec_.attempt[i];
        rec_.recovered[i]                 = rec_.recovered[i] == 0xFFFFU
                                            ? rec_.recovered[i]
                                            : static_cast<std::uint16_t>(rec_.recovered[i] + 1);
        auto const uptime                 = uptimeMsOf(now);
        auto&      r                      = rec_.record;
        r.total                           = r.total + 1;
        r.lastCheck                       = static_cast<std::uint32_t>(i);
        r.lastNameHash                    = fnv1a(Cfg::names[i]);
        r.lastUptimeMs                    = uptime;
        storeRecord();
#ifdef USE_UC_LOG
        UC_LOG_I("health: check '{}' back after recovery {} ({} this run, {} for this check)",
                 Cfg::names[i],
                 rungs,
                 r.total,
                 rec_.recovered[i]);
#endif
    }

    // check i goes to the policy; say how many rungs it had
    static void escalating(std::size_t i) {
        if(Cfg::attempts[i] == 0) { return; }
        auto const rungs              = static_cast<std::uint32_t>(rec_.attempt[i]);
        rec_.record.escalatedAttempts = rungs;
        storeRecord();
#ifdef USE_UC_LOG
        UC_LOG_E("health: check '{}' still silent after {} recovery attempt(s): escalating",
                 Cfg::names[i],
                 rungs);
#endif
    }

    // logReport()'s part for the recovery record (takes it)
    static void logRecoveries() {
        [[maybe_unused]] auto const r = takeLastRecoveries();
        if(!r) { return; }
#ifdef USE_UC_LOG
        auto const nameOf = [](std::uint32_t check, std::uint32_t hash) {
            return check < N && fnv1a(Cfg::names[check]) == hash
                   ? Cfg::names[check]
                   : std::string_view{"(not in this build)"};
        };
        auto const nameAt = [](std::uint32_t check) {
            return check < N ? Cfg::names[check] : std::string_view{"(not in this build)"};
        };
        if(r->total != 0) {
            UC_LOG_I(
              "health: the run before this one recovered {} time(s) by onStarved, last check '{}' "
              "{} ms after boot",
              r->total,
              nameOf(r->lastCheck, r->lastNameHash),
              r->lastUptimeMs);
        }
        if(r->inProgress != 0) {
            auto const check = inProgressCheck(r->inProgress);
            UC_LOG_E(
              "health: the run before this one was reset while onStarved of '{}' ran (attempt {} "
              "of {})",
              nameAt(check),
              inProgressAttempt(r->inProgress),
              check < N ? Cfg::attempts[check] : 0U);
        }
        if(r->escalatedAttempts != 0) {
            UC_LOG_E(
              "health: the run before this one escalated a check after {} recovery attempt(s)",
              r->escalatedAttempts);
        }
#endif
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
    static inline Detail::RecoveryState<N, Cfg::anyRecovery> rec_{};
};
}   // namespace Kvasir::Health
