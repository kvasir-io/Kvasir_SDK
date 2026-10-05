#pragma once
// A deadline executor: many timed callbacks on ONE hardware compare, run in the main loop (thread mode), and a main
// loop that sleeps until the next deadline or interrupt.
//
//     struct ExecConfig {
//         using Clock = HW::TimerClock;                                         // runs while the core sleeps
//         using Wake  = Kvasir::Timer::Alarm<HW::TimerClock, 3, &Kvasir::noWork>;   // the one comparator
//     };
//     using Exec = Kvasir::Executor<ExecConfig>;
//
//     constexpr auto HeartbeatPeriod = 250ms;
//     struct Heartbeat {                       // periodic work declared in its own type, like MainLoop work
//         static void tick();
//         using Extends = Kvasir::Startup::Extend<Kvasir::Hook::Every<HeartbeatPeriod>, &Heartbeat::tick>;
//     };
//     Exec::Timer    settle{[] { startMeasurement(); }};        // run-time timers
//     Exec::Deferred rxReady{[] { parseFrames(); }};            // interrupt -> loop, coalescing: Exec::post(rxReady)
//     Exec::in(settle, 3ms); Exec::every(sample, 10ms, Kvasir::Repeat::skip);
//
//     int main() {
//         Exec::adopt<Startup>();             // links the Hook::Every entries; logs the entries that keep it awake
//         while(true) { Exec::sleep(Startup::runTurn<Kvasir::Hook::MainLoop>()); }   // Exec::turn() is an entry
//     }
//
// A MainLoop function says when it needs the next turn by its return type (Hooks.hpp): Kvasir::Turn, bool ("did
// work") or void (busy: keeps the loop awake; adopt() names those once at boot).
//
// A periodic entry's period is a reference to a constexpr duration object (a duration cannot be a template argument:
// its member is private).
//
// Callbacks run in thread mode, from turn(): the Wake alarm's ISR only ends the sleep. Repeat mirrors Every
// (Periodic.hpp): catchUp (each missed period runs, maxPerTurn per turn), skip (once, the missed ones counted in
// missed()), rebase (next = now + period). Arming, cancel and post work from any ISR of the core; post from the other
// core too (sev() wakes it).
//
// Sleep (Config::Sleep): Wfe (the default with a Wake) - an interrupt between the check and the WFE ends in an
// exception return, which sets the event register (Armv6-M B1.5.18, Armv8-M "WFE"), so the WFE falls through; a
// check uses plain loads, since an exclusive store sets this core's event register on Armv8-M (Barrier.hpp).
// WfiMasked - PRIMASK, check, WFI: a pending interrupt ends WFI despite PRIMASK; sees only interrupts after the mask,
// so an ISR that left work for an entry calls kick(). Spin - no sleep (the default without a Wake). A sleeping
// executor needs a clock that runs while the core sleeps (`static constexpr bool runsAsleep = true` - the RP TIMER;
// not SysTick, which stops with CLK_CPU on the SAM chips).
#include "kvasir/Atomic/CriticalSection.hpp"
#include "kvasir/StartUp/Hooks.hpp"
#include "kvasir/Util/Periodic.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#ifdef __arm__
    #include "core/Barrier.hpp"
#endif
#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"
#endif

namespace Kvasir {
/// An alarm ISR whose only job is to end the sleep.
inline void noWork() {}

enum class Repeat : std::uint8_t { once, catchUp, skip, rebase };

namespace Detail { inline constexpr std::chrono::microseconds NoPhase{0}; }   // namespace Detail

namespace Hook {
    /// Periodic work: `Extend<Hook::Every<Period>, &fn>` with `constexpr auto Period = 250ms;` (any duration type),
    /// optionally a phase (the first run that long after adopt()) and what a late run does.
    template<auto const& Period,
             auto const& Phase = Kvasir::Detail::NoPhase,
             Repeat      R     = Repeat::catchUp>
    struct Every {
        using Signature              = void();
        static constexpr auto period = Period;
        static constexpr auto phase  = Phase;
        static constexpr auto repeat = R;
    };
}   // namespace Hook

namespace Detail {
    template<typename H>
    inline constexpr bool isEveryHook = false;

    template<auto const& P, auto const& Ph, Repeat R>
    inline constexpr bool isEveryHook<Hook::Every<P, Ph, R>> = true;

    // the type's name as the compiler spells it, from __PRETTY_FUNCTION__ (clang "[T = X]", gcc "[with T = X; ...]")
    template<typename T>
    constexpr std::string_view typeNameOf() {
        std::string_view const f = __PRETTY_FUNCTION__;
        auto const             b = f.find("T = ");
        if(b == std::string_view::npos) { return f; }
        auto const e = f.find_first_of(";]", b + 4);
        return f.substr(b + 4, e == std::string_view::npos ? std::string_view::npos : e - b - 4);
    }

    template<typename C>
    concept ClockRunsAsleep = requires {
        { C::runsAsleep } -> std::convertible_to<bool>;
    } && C::runsAsleep;
}   // namespace Detail

/// No hardware compare: timers are polled from turn(), the loop never sleeps.
struct NoWake {};

template<typename W, typename TP>
concept WakeSource = requires(TP t) {
    W::arm(t);
    W::disarm();
};

namespace Sleep {
    struct Spin {
        static constexpr bool sleeps = false;

        template<typename F>
        static void until(F&&) {}
    };

#ifdef __arm__
    struct Wfe {
        static constexpr bool sleeps = true;

        template<typename F>
        static void until(F&& stillIdle) {
            if(stillIdle()) { Core::wfe(); }
        }
    };

    struct WfiMasked {
        static constexpr bool sleeps = true;

        template<typename F>
        static void until(F&& stillIdle) {
            Nvic::InterruptGuard<Nvic::Global> const guard{};
            if(stillIdle()) { Core::wfi(); }
        }
    };
#endif
}   // namespace Sleep

template<typename Config>
struct Executor {
    using Clock      = typename Config::Clock;
    using time_point = typename Clock::time_point;
    using duration   = typename Clock::duration;

private:
    static constexpr auto wakeOf() {
        if constexpr(requires { typename Config::Wake; }) {
            return static_cast<typename Config::Wake*>(nullptr);
        } else {
            return static_cast<NoWake*>(nullptr);
        }
    }

public:
    using Wake                    = std::remove_pointer_t<decltype(wakeOf())>;
    static constexpr bool HasWake = !std::is_same_v<Wake, NoWake>;

private:
    static constexpr auto sleepOf() {
        if constexpr(requires { typename Config::Sleep; }) {
            return static_cast<typename Config::Sleep*>(nullptr);
#ifdef __arm__
        } else if constexpr(HasWake) {
            return static_cast<Sleep::Wfe*>(nullptr);
#endif
        } else {
            return static_cast<Sleep::Spin*>(nullptr);
        }
    }

public:
    using SleepPolicy = std::remove_pointer_t<decltype(sleepOf())>;

    static constexpr std::uint32_t MaxPerTurn = [] {
        if constexpr(requires { Config::maxPerTurn; }) {
            return std::uint32_t{Config::maxPerTurn};
        } else {
            return std::uint32_t{8};
        }
    }();
    static constexpr bool WithStats = [] {
        if constexpr(requires { Config::stats; }) {
            return bool{Config::stats};
        } else {
            return true;
        }
    }();
    static constexpr duration MaxSleep = [] {
        duration d = std::chrono::duration_cast<duration>(std::chrono::minutes{30});
        if constexpr(requires { Config::maxSleep; }) {
            d = std::chrono::duration_cast<duration>(Config::maxSleep);
        }
        if constexpr(requires { Wake::maxAhead; }) {
            d = std::min(d, std::chrono::duration_cast<duration>(Wake::maxAhead));
        }
        return d;
    }();

    static_assert(PollClock<Clock>,
                  "Config::Clock: a PollClock (now(), time_point, duration)");
    static_assert(!HasWake
                    || WakeSource<Wake,
                                  time_point>,
                  "Config::Wake needs arm(time_point) and disarm() on the executor's clock");
    static_assert(
      !SleepPolicy::sleeps || HasWake,
      "a sleeping executor needs a Wake: nothing would end the sleep at the next deadline");
    static_assert(!SleepPolicy::sleeps || Detail::ClockRunsAsleep<Clock>,
                  "this clock does not run while the core sleeps (SysTick on the SAM chips stops "
                  "with CLK_CPU): "
                  "use a clock with `static constexpr bool runsAsleep = true` (the RP TIMER)");

    // A plain function (a lambda without captures converts): a Timer or Deferred at namespace scope is then built
    // at compile time, with no static constructor.
    using Callback = void (*)();

    class Timer {
    public:
        constexpr Timer() = default;

        constexpr explicit Timer(Callback f) : cb_{f} {}

        Timer(Timer const&)            = delete;
        Timer& operator=(Timer const&) = delete;

        [[nodiscard]] bool armed() const { return linked_; }

        [[nodiscard]] time_point due() const { return due_; }

        /// Repeat::skip: the periods passed over since arming.
        [[nodiscard]] std::uint32_t missed() const { return missed_; }

        void setCallback(Callback cb) { cb_ = cb; }

    private:
        friend Executor;
        Timer*        next_{};
        time_point    due_{};
        duration      period_{};
        Callback      cb_{};
        std::uint32_t missed_{};
        Repeat        repeat_{Repeat::once};
        bool          linked_{};
    };

    /// Interrupt -> loop: posted twice before it runs, it runs once.
    class Deferred {
    public:
        constexpr explicit Deferred(Callback f) : cb_{f} {}

        Deferred(Deferred const&)            = delete;
        Deferred& operator=(Deferred const&) = delete;

    private:
        friend Executor;
        Deferred* next_{};
        Callback  cb_{};
        bool      posted_{};
    };

    struct Stats {
        std::uint32_t callbacks;   // timer and deferred callbacks run
        std::uint32_t wakes;       // sleeps ended
        std::uint32_t turns;       // turn() calls
        duration      worstLate;   // the latest a timer ran after its due time
        duration      slept;
        duration      awake;
    };

    // Arming: thread mode or an ISR of this core.
    static void at(Timer&     t,
                   time_point when) {
        section([&] {
            unlink(t);
            t.due_    = when;
            t.period_ = duration::zero();
            t.repeat_ = Repeat::once;
            t.missed_ = 0;
            link(t);
        });
        kick();
    }

    template<typename Rep,
             typename Period>
    static void in(Timer&                        t,
                   std::chrono::duration<Rep,
                                         Period> d) {
        at(t, Clock::now() + std::chrono::duration_cast<duration>(d));
    }

    template<typename Rep,
             typename Period>
    static void every(Timer&                        t,
                      std::chrono::duration<Rep,
                                            Period> period,
                      Repeat                        r     = Repeat::catchUp,
                      time_point                    first = Clock::now()) {
        auto const p = std::chrono::duration_cast<duration>(period);
        section([&] {
            unlink(t);
            t.due_    = first;
            t.period_ = p > duration::zero() ? p : duration{1};
            t.repeat_ = r == Repeat::once ? Repeat::catchUp : r;
            t.missed_ = 0;
            link(t);
        });
        kick();
    }

    /// True if it was armed.
    static bool cancel(Timer& t) {
        return section([&] {
            bool const was = t.linked_;
            unlink(t);
            return was;
        });
    }

    /// Any context, either core.
    static void post(Deferred& d) {
        section([&] {
            if(d.posted_) { return; }
            d.posted_ = true;
            d.next_   = posted_;
            posted_   = &d;
        });
        kick();
    }

    /// "An interrupt left work for an entry": the next sleep() does not sleep (Sleep::WfiMasked needs it).
    static void kick() {
        kicked_.store(true, std::memory_order_relaxed);
#ifdef __arm__
        Core::sev();
#endif
    }

    /// Posted Deferreds, then due timers (at most MaxPerTurn callbacks); returns when it next needs the loop.
    static Turn turn() {
        if constexpr(WithStats) { ++stats_.turns; }
        std::uint32_t ran = drainPosted();
        auto          now = Clock::now();
        while(ran < MaxPerTurn) {
            Timer* const t = section([&]() -> Timer* {
                if(head_ == nullptr || head_->due_ > now) { return nullptr; }
                Timer* const h = head_;
                head_          = h->next_;
                h->linked_     = false;
                h->next_       = nullptr;
                relink(
                  *h,
                  now);   // a repeating one goes back in BEFORE the call: it may cancel or re-arm itself
                return h;
            });
            if(t == nullptr) {
                auto const fresh
                  = Clock::now();   // re-read only when the head is not due by the old reading
                auto const head = headDue();
                if(head > fresh) {
                    return head == time_point::max() ? Turn::idle() : Turn::within(head - fresh);
                }
                now = fresh;
                continue;
            }
            if(t->cb_ != nullptr) { t->cb_(); }
            ++ran;
        }
        return Turn::busy();   // MaxPerTurn reached: the rest next turn
    }

    using Extends = Startup::Extend<Hook::MainLoop, &Executor::turn>;

    /// Until min(next due, hint), or an interrupt / event.
    static void sleep(Turn hint) {
        if constexpr(SleepPolicy::sleeps) {
            if(hint.isBusy() || kicked_.load(std::memory_order_relaxed)) {
                kicked_.store(false, std::memory_order_relaxed);
                return;
            }
            auto const now  = Clock::now();
            auto       wake = std::min(headDue(), now + MaxSleep);
            if(!hint.isIdle()) {
                wake = std::min(wake, now + std::chrono::ceil<duration>(hint.in()));
            }
            if(wake <= now) { return; }
            Wake::arm(wake);
            if constexpr(requires { Config::SleepHook::enter(); }) { Config::SleepHook::enter(); }
            SleepPolicy::until(
              [] { return !kicked_.load(std::memory_order_relaxed) && postedPending() == false; });
            Wake::disarm();
            kicked_.store(false, std::memory_order_relaxed);
            if constexpr(requires { Config::SleepHook::leave(); }) { Config::SleepHook::leave(); }
            if constexpr(WithStats) {
                auto const after = Clock::now();
                ++stats_.wakes;
                stats_.slept += after - now;
                stats_.awake += now - lastWake_;
                lastWake_ = after;
            }
        } else {
            (void)hint;
            kicked_.store(false, std::memory_order_relaxed);
        }
    }

    /// Link every `Extend<Hook::Every<...>>` of the list's peripherals, and (log builds) name once the MainLoop
    /// functions that return void: they keep the loop awake.
    template<typename StartupT>
    static void adopt() {
        using All =
          typename Startup::Detail::AllExtendsOfList<typename StartupT::LocalPeripherals>::type;
        auto const now = Clock::now();
        adoptEach(static_cast<All*>(nullptr), now);
        logAwake<Kvasir::Hook::MainLoop>(static_cast<All*>(nullptr));
    }

    /// Taken and cleared.
    static Stats takeStats() {
        auto const s = stats_;
        stats_       = {};
        return s;
    }

    /// The time the earliest armed timer is due, time_point::max() if none.
    [[nodiscard]] static time_point headDue() {
        return section([] { return head_ == nullptr ? time_point::max() : head_->due_; });
    }

    /// Forget every timer and posted Deferred (host tests).
    static void reset() {
        section([] {
            while(head_ != nullptr) {
                Timer* const t = head_;
                head_          = t->next_;
                t->next_       = nullptr;
                t->linked_     = false;
            }
            while(posted_ != nullptr) {
                Deferred* const d = posted_;
                posted_           = d->next_;
                d->next_          = nullptr;
                d->posted_        = false;
            }
        });
        kicked_.store(false, std::memory_order_relaxed);
        stats_ = {};
    }

private:
    struct SectionTag {};

    template<typename F>
    static decltype(auto) section(F&& f) {
        return Kvasir::criticalSection<SectionTag>(std::forward<F>(f));
    }

    // sorted insert, equal times after the ones already in; under the section
    static void link(Timer& t) {
        Timer** p = &head_;
        while(*p != nullptr && (*p)->due_ <= t.due_) { p = &(*p)->next_; }
        t.next_   = *p;
        *p        = &t;
        t.linked_ = true;
    }

    static void unlink(Timer& t) {
        if(!t.linked_) { return; }
        for(Timer** p = &head_; *p != nullptr; p = &(*p)->next_) {
            if(*p == &t) {
                *p = t.next_;
                break;
            }
        }
        t.next_   = nullptr;
        t.linked_ = false;
    }

    // a timer that ran: note its lateness, give a repeating one its next due time and link it again
    static void relink(Timer&     t,
                       time_point now) {
        if constexpr(WithStats) {
            stats_.worstLate = std::max(stats_.worstLate, duration{now - t.due_});
            ++stats_.callbacks;
        }
        switch(t.repeat_) {
        case Repeat::once:    return;
        case Repeat::catchUp: t.due_ += t.period_; break;
        case Repeat::skip:
            {
                auto const k = static_cast<std::uint32_t>((now - t.due_) / t.period_) + 1U;
                t.due_ += t.period_ * k;
                t.missed_ += k - 1U;
                break;
            }
        case Repeat::rebase: t.due_ = now + t.period_; break;
        }
        link(t);
    }

    // the posted ones, in post order; each cleared before its call, so it may post itself again
    static std::uint32_t drainPosted() {
        Deferred* list    = section([] {
            Deferred* l = posted_;
            posted_     = nullptr;
            return l;
        });
        Deferred* ordered = nullptr;   // reverse: the stack holds the last post first
        while(list != nullptr) {
            Deferred* const n = list->next_;
            list->next_       = ordered;
            ordered           = list;
            list              = n;
        }
        std::uint32_t ran = 0;
        while(ordered != nullptr) {
            Deferred* const d = ordered;
            ordered           = d->next_;
            section([&] {
                d->next_   = nullptr;
                d->posted_ = false;
            });
            if(d->cb_ != nullptr) { d->cb_(); }
            ++ran;
            if constexpr(WithStats) { ++stats_.callbacks; }
        }
        return ran;
    }

    // a plain load, for the sleep check (no exclusive store: it would set the event register)
    static bool postedPending() {
        return *static_cast<Deferred* const volatile*>(&posted_) != nullptr;
    }

    template<typename E>
    static constinit inline Timer staticTimer{[] { E::fn(); }};

    template<typename... Es>
    static void adoptEach(brigand::list<Es...>*,
                          time_point now) {
        (
          [&] {
              if constexpr(Detail::isEveryHook<typename Es::Hook>) {
                  using H = typename Es::Hook;
                  every(staticTimer<Es>,
                        H::period,
                        H::repeat,
                        now + std::chrono::duration_cast<duration>(H::phase));
              }
          }(),
          ...);
    }

    template<typename Hook,
             typename... Es>
    static void logAwake(brigand::list<Es...>*) {
#ifdef USE_UC_LOG
        constexpr std::size_t n = (std::size_t{Startup::Detail::returnsVoidOn<Hook, Es>} + ... + 0);
        if constexpr(n != 0 && SleepPolicy::sleeps) {
            UC_LOG_I(
              "executor: {} main-loop function(s) return void and keep the loop awake (return "
              "Kvasir::Turn "
              "or bool to let it sleep):",
              n);
            (
              [] {
                  if constexpr(Startup::Detail::returnsVoidOn<Hook, Es>) {
                      UC_LOG_I("executor:   {}", Kvasir::Detail::typeNameOf<Es>());
                  }
              }(),
              ...);
        }
#endif
    }

    static inline Timer*            head_{};
    static inline Deferred*         posted_{};
    static inline std::atomic<bool> kicked_{};
    static inline Stats             stats_{};
    static inline time_point        lastWake_{};
};

/// A Timer that cancels itself when it goes out of scope (a timer on a stack frame).
template<typename Exec>
class ScopedTimer : public Exec::Timer {
public:
    using Exec::Timer::Timer;

    ~ScopedTimer() { Exec::cancel(*this); }
};

/// A Startup-list entry: every Period, the executor's statistics as one log line with metrics.
template<typename Exec, auto const& Period>
struct ExecutorReport {
    /// Everything reported so far, summed (worstLate: the worst of all).
    [[nodiscard]] static typename Exec::Stats total() { return total_; }

    static void report() {
        auto const s = Exec::takeStats();
        total_.callbacks += s.callbacks;
        total_.wakes += s.wakes;
        total_.turns += s.turns;
        total_.worstLate = std::max(total_.worstLate, s.worstLate);
        total_.slept += s.slept;
        total_.awake += s.awake;
#ifdef USE_UC_LOG
        using us                  = std::chrono::microseconds;
        auto const          slept = std::chrono::duration_cast<us>(s.slept).count();
        auto const          awake = std::chrono::duration_cast<us>(s.awake).count();
        std::uint32_t const sleepPermille
          = slept + awake > 0 ? static_cast<std::uint32_t>((slept * 1000) / (slept + awake)) : 0U;
        UC_LOG_I("executor: {} callbacks, {} turns, {} wakes, worst lateness {}, slept {}.{}%",
                 s.callbacks,
                 s.turns,
                 s.wakes,
                 std::chrono::duration_cast<us>(s.worstLate),
                 sleepPermille / 10,
                 sleepPermille % 10);
#endif
    }

    using Extends = Startup::Extend<Hook::Every<Period>, &ExecutorReport::report>;

private:
    static inline typename Exec::Stats total_{};
};
}   // namespace Kvasir
