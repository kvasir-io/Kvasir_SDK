#pragma once
// The recovery ladder of the health supervisor (kvasir/Health/Health.hpp, "A local recovery before the policy"): the
// onStarved probes, the call into a check's onStarved, the ladder state, and Detail::Ladder<Sup>, its functions on
// the supervisor's statics. An image without any onStarved instantiates none of it. Included by Health/Supervisor.hpp.
//
// A supervisor serviced from an interrupt (so a hung main loop is a starved check like any other) sets
// `deferRecoveries = true` in its Config: service() then only REQUESTS a recovery (starts the rung's window, counts
// and logs it), and the main loop runs it with Supervisor::runRecoveries() - thread mode, one onStarved per call,
// same core as service(), same contract. The window starts at the request; a request not run before it ends goes
// on like a silent check (next rung, then the policy). Recovery::cannot escalates at the next service() unless the
// check beat meanwhile. A beat, resume() or a fresh OnStarvation::Log window drops a request; a policy that stops
// feeding drops them all.
//
// The watchdog bounds the main loop, onStarved included: period x 2 + recoveryBudget must stay below its timeout
// (a static_assert; not with deferRecoveries, where the interrupt feeds it and the loop's own check bounds
// onStarved). From a check's last beat to the reset under StopFeeding it is at most
// maxInterval + recoveryAttempts x recoveryWindow + (recoveryAttempts + 1) x period + the watchdog's timeout:
// Supervisor::worstToEscalationMs<C>(). Without any onStarved in the image nothing of this is compiled in.
#include "kvasir/Atomic/CriticalSection.hpp"
#include "kvasir/Health/Check.hpp"
#include "kvasir/Health/HealthRecords.hpp"
#include "kvasir/Util/Config.hpp"
#ifdef __arm__
    #include "core/Special.hpp"
    #include "kvasir/Runtime/Check.hpp"
#endif

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir::Health::Detail {
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
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      KVASIR_CONFIG_OR(C, recoveryWindow, C::maxInterval));
}

#ifdef __arm__
// the exception number in IPSR: 0 in thread mode (DDI0419E B1.4.2, DDI0553B.y B3.4)
inline std::uint32_t exceptionNumber() {
    // IPSR & 0x1FF (core/Special.hpp)
    return Kvasir::Core::exceptionNumber();
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

// deferRecoveries: the hand-off between service() and runRecoveries(), under criticalSection<RecoveryTag>;
// nothing (never odr-used) without it
template<std::size_t N, bool On>
struct DeferState {};

template<std::size_t N>
struct DeferState<N, true> {
    // mask: checks with a rung for runRecoveries() to run
    std::uint32_t requested{};
    // mask: their onStarved returned Recovery::cannot, for the next service() to escalate
    std::uint32_t refused{};
    // the requested rung's Attempt::n
    std::array<std::uint8_t, N> rung{};
    // silence and limit at the request (the window it started reset them): what a refusal escalates with
    std::array<std::uint32_t, N> silentMs{};
    std::array<std::uint32_t, N> limitMs{};
};

struct RecoveryTag {};

// The ladder's functions, on the supervisor's state (Sup's statics: their names are what the bench reads). A
// friend of Supervisor, instantiated only where some check has an onStarved.
template<typename Sup>
struct Ladder {
    using Cfg                                    = typename Sup::Cfg;
    using Clock                                  = typename Sup::ClockType;
    static constexpr std::size_t n               = Sup::n;
    static constexpr bool        deferRecoveries = Sup::deferRecoveries;

    // service()'s hooks, always inlined: the turn compiles as the code did inline
    [[gnu::always_inline]] static void beginTurn() { Sup::rec_.calledThisTurn = false; }

    // check i beat
    [[gnu::always_inline]] static void onBeat(std::size_t const                i,
                                              std::uint32_t const              bit,
                                              typename Clock::time_point const now) {
        if((Sup::rec_.recovering & bit) != 0) { recovered(i, now); }
        Sup::rec_.attempt[i] = 0;   // progress: the ladder starts over
        if constexpr(deferRecoveries) { dropRequests(bit); }
    }

    // check i is past its window: true if it recovers (a rung now, or its rung comes next turn)
    [[gnu::always_inline]] static bool rescue(std::size_t const i) {
        bool rescued = false;
        if(Sup::rec_.attempt[i] < Cfg::attempts[i] && !Sup::latched_) {
            if(Sup::rec_.calledThisTurn) {
                rescued = true;   // its rung comes next turn
            } else {
                Sup::rec_.calledThisTurn = true;
                rescued                  = recover(i);
            }
        }
        return rescued;
    }

    // resume<C>(): a fresh ladder
    [[gnu::always_inline]] static void onResume(std::size_t const   i,
                                                std::uint32_t const bit) {
        if((Sup::rec_.recovering & bit) != 0) { Sup::limitMs_[i] = Cfg::allowedMs[i]; }
        Sup::rec_.recovering = Sup::rec_.recovering & ~bit;
        Sup::rec_.attempt[i] = 0;
        if constexpr(deferRecoveries) { dropRequests(bit); }
    }

    // deferRecoveries, a policy that stops feeding: no recovery runs while the reset (or the panic) comes
    [[gnu::always_inline]] static void onLatched() {
        if(Sup::latched_) { dropRequests(~0U); }
    }

    // OnStarvation::Log's fresh window: and a fresh ladder for the next silence
    [[gnu::always_inline]] static void onFreshWindow(std::size_t const i) {
        // i is a check's index (from service()'s loop); gcc does not see that through the optional and the
        // starvation reaction and warns of a write past attempt (-Wstringop-overflow, gcc 16 -O3 under UBSan)
        [[assume(i < n)]];
        Sup::limitMs_[i]     = Cfg::allowedMs[i];
        Sup::rec_.recovering = Sup::rec_.recovering & ~(1U << i);
        Sup::rec_.attempt[i] = 0;
        if constexpr(deferRecoveries) { dropRequests(1U << i); }
    }

    // Supervisor::runRecoveries()'s body
    [[gnu::always_inline]] static bool runRecoveries() {
        struct Taken {
            std::size_t  i;
            std::uint8_t n;
        };

        // the check and its rung together: a newer rung service() requests after this is the next call's
        auto const taken = criticalSection<RecoveryTag>([]() -> std::optional<Taken> {
            if(Sup::defer_.requested == 0) { return std::nullopt; }
            auto const i = static_cast<std::size_t>(std::countr_zero(Sup::defer_.requested));
            Sup::defer_.requested = Sup::defer_.requested & ~(1U << i);
            auto const rung       = Sup::defer_.rung[i];
            // before the call: a reset inside it is named at the next boot
            Sup::rec_.record.inProgress
              = recoveryInProgress(static_cast<std::uint32_t>(i), rung + 1U);
            storeRecordNow();
            return Taken{i, rung};
        });
        if(!taken) { return false; }
        Recovery const r = callTimed(taken->i, taken->n);
        criticalSection<RecoveryTag>([i = taken->i, r] {
            Sup::rec_.record.inProgress = 0;
            storeRecordNow();
            // only while still recovering: a beat during the call (service() preempting it) ended the recovery,
            // and a stale refusal would escalate a check that is back
            if(r == Recovery::cannot && (Sup::rec_.recovering & (1U << i)) != 0) {
                Sup::defer_.refused = Sup::defer_.refused | (1U << i);
            }
            return 0;
        });
        return true;
    }

    // Supervisor::pendingRecoveries()'s body
    [[gnu::always_inline]] static std::uint32_t pendingRecoveries() {
        return criticalSection<RecoveryTag>([] { return Sup::defer_.requested; });
    }

    // one store for every change (a single CRC loop in the image); the RAM copy is the truth
    [[gnu::noinline]] static void storeRecordNow() { lastRecovery.store(Sup::rec_.record); }

    // deferRecoveries: runRecoveries() stores it too, from thread mode
    static void storeRecord() {
        if constexpr(deferRecoveries) {
            criticalSection<RecoveryTag>([] {
                storeRecordNow();
                return 0;
            });
        } else {
            storeRecordNow();
        }
    }

    // deferRecoveries: forget the requests and refusals of these checks (a beat, resume, a fresh OnStarvation::Log
    // window, a policy that stops feeding)
    static void dropRequests(std::uint32_t const bits) {
        criticalSection<RecoveryTag>([bits] {
            Sup::defer_.requested = Sup::defer_.requested & ~bits;
            Sup::defer_.refused   = Sup::defer_.refused & ~bits;
            return 0;
        });
    }

    // deferRecoveries: the check's onStarved returned Recovery::cannot (takes it, and a request of a newer rung);
    // false without deferRecoveries
    static bool takeRefused([[maybe_unused]] std::uint32_t const bit) {
        if constexpr(!deferRecoveries) {
            return false;
        } else {
            return criticalSection<RecoveryTag>([bit] {
                bool const was      = (Sup::defer_.refused & bit) != 0;
                Sup::defer_.refused = Sup::defer_.refused & ~bit;
                if(was) { Sup::defer_.requested = Sup::defer_.requested & ~bit; }
                return was;
            });
        }
    }

    // deferRecoveries: a refused check's silence and limit as they were at its request, plus the silence since
    static void refusedSilence([[maybe_unused]] std::size_t const i) {
        if constexpr(deferRecoveries) {
            Sup::silentMs_[i] = Sup::defer_.silentMs[i] + Sup::silentMs_[i];
            Sup::limitMs_[i]  = Sup::defer_.limitMs[i];
        }
    }

    // onStarved of check i at a rung, timed against Sup::recoveryBudget
    static Recovery callTimed(std::size_t const  i,
                              std::uint8_t const rung) {
        auto const     t0 = Clock::now();
        Recovery const r  = Cfg::onStarved[i](Attempt{.n = rung, .of = Cfg::attempts[i]});
        auto const     dt = Clock::now() - t0;
        if(dt > Sup::recoveryBudget) {
            Sup::rec_.overruns = Sup::rec_.overruns + 1;
#ifdef USE_UC_LOG
            UC_LOG_W("health: onStarved of '{}' took {} us (budget {} us)",
                     Cfg::names[i],
                     static_cast<std::uint32_t>(
                       std::chrono::duration_cast<std::chrono::microseconds>(dt).count()),
                     static_cast<std::uint32_t>(
                       std::chrono::duration_cast<std::chrono::microseconds>(Sup::recoveryBudget)
                         .count()));
#endif
        }
        if(r == Recovery::cannot) {
#ifdef USE_UC_LOG
            UC_LOG_W("health: onStarved of '{}' cannot recover", Cfg::names[i]);
#endif
        }
        return r;
    }

    // check i is past its window with a rung left: call its onStarved; true if it is recovering now
    static bool recover(std::size_t i) {
        std::uint32_t const bit  = 1U << i;
        auto const          rung = Sup::rec_.attempt[i];
        auto const          of   = Cfg::attempts[i];
        Sup::rec_.attempt[i]     = static_cast<std::uint8_t>(rung + 1);
        if constexpr(deferRecoveries) {
#ifdef USE_UC_LOG
            UC_LOG_W("health: check '{}' silent {} ms (allowed {} ms): recovery {} of {} requested",
                     Cfg::names[i],
                     Sup::silentMs_[i],
                     Sup::limitMs_[i],
                     rung + 1U,
                     of);
#endif
            // the main loop runs it (runRecoveries()); the window starts now
            criticalSection<RecoveryTag>([i, rung, bit] {
                Sup::defer_.rung[i]     = rung;
                Sup::defer_.silentMs[i] = Sup::silentMs_[i];
                Sup::defer_.limitMs[i]  = Sup::limitMs_[i];
                Sup::defer_.requested   = Sup::defer_.requested | bit;
                return 0;
            });
        } else {
#ifdef USE_UC_LOG
            UC_LOG_W("health: check '{}' silent {} ms (allowed {} ms): recovery {} of {}",
                     Cfg::names[i],
                     Sup::silentMs_[i],
                     Sup::limitMs_[i],
                     rung + 1U,
                     of);
#endif
            // before the call: a reset inside it is named at the next boot
            Sup::rec_.record.inProgress
              = recoveryInProgress(static_cast<std::uint32_t>(i), rung + 1U);
            storeRecord();
            Recovery const r            = callTimed(i, rung);
            Sup::rec_.record.inProgress = 0;
            storeRecord();
            if(r == Recovery::cannot) {
                Sup::rec_.attempt[i] = of;   // the ladder is spent: escalate in this turn
                return false;
            }
        }
        Sup::rec_.recovering = Sup::rec_.recovering | bit;
        Sup::silentMs_[i]    = 0;
        Sup::limitMs_[i]     = Cfg::recoveryWindowMs[i];
        return true;
    }

    // check i beat inside its recovery window
    static void recovered(std::size_t                i,
                          typename Clock::time_point now) {
        Sup::rec_.recovering              = Sup::rec_.recovering & ~(1U << i);
        [[maybe_unused]] auto const rungs = Sup::rec_.attempt[i];
        Sup::rec_.recovered[i] = Sup::rec_.recovered[i] == 0xFFFFU
                                 ? Sup::rec_.recovered[i]
                                 : static_cast<std::uint16_t>(Sup::rec_.recovered[i] + 1);
        auto const uptime      = Sup::uptimeMsOf(now);
        auto&      r           = Sup::rec_.record;
        r.total                = r.total + 1;
        r.lastCheck            = static_cast<std::uint32_t>(i);
        r.lastNameHash         = fnv1a(Cfg::names[i]);
        r.lastUptimeMs         = uptime;
        storeRecord();
#ifdef USE_UC_LOG
        UC_LOG_I("health: check '{}' back after recovery {} ({} this run, {} for this check)",
                 Cfg::names[i],
                 rungs,
                 r.total,
                 Sup::rec_.recovered[i]);
#endif
    }

    // check i goes to the policy; say how many rungs it had
    static void escalating(std::size_t i) {
        if(Cfg::attempts[i] == 0) { return; }
        auto const rungs                   = static_cast<std::uint32_t>(Sup::rec_.attempt[i]);
        Sup::rec_.record.escalatedAttempts = rungs;
        storeRecord();
#ifdef USE_UC_LOG
        UC_LOG_E("health: check '{}' still silent after {} recovery attempt(s): escalating",
                 Cfg::names[i],
                 rungs);
#endif
    }

    // logReport()'s part for the recovery record (takes it)
    static void logRecoveries() {
        [[maybe_unused]] auto const r = Sup::takeLastRecoveries();
        if(!r) { return; }
#ifdef USE_UC_LOG
        auto const nameOf = [](std::uint32_t check, std::uint32_t hash) {
            return check < n && fnv1a(Cfg::names[check]) == hash
                   ? Cfg::names[check]
                   : std::string_view{"(not in this build)"};
        };
        auto const nameAt = [](std::uint32_t check) {
            return check < n ? Cfg::names[check] : std::string_view{"(not in this build)"};
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
              check < n ? Cfg::attempts[check] : 0U);
        }
        if(r->escalatedAttempts != 0) {
            UC_LOG_E(
              "health: the run before this one escalated a check after {} recovery attempt(s)",
              r->escalatedAttempts);
        }
#endif
    }
};
}   // namespace Kvasir::Health::Detail
