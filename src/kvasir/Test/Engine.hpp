#pragma once
// The part of a test run that Runner (a test image, Test/Runner.hpp) and OnRequest (an application's self-test,
// Test/OnRequest.hpp) share: the configuration, one test's run with its log lines, the loop over a run, its end.
//
// Config (derive from DefaultConfig, set Clock):
//   Clock           a Kvasir clock (now(), a duration): the tests' times, the host wait
//   Watchdog        NoWatchdog, or ChipWatchdog<W> over the chip's Kvasir::Watchdog<...>: armed for the
//                   Outcome::watchdog tests only (a hung `returns` test is the host's timeout - a reset nobody announced
//                   would fall into a printer's session, which parks an RP2040 in its boot ROM)
//   Cycles          void (ClockCycles<Clock>), DwtCycles or CoreCycles<Clock> (Test/Perf.hpp): the perf tests' clock
//   hostWait        how long a test image waits at boot for the host's request when a printer listens
//   perfIterations  a perf test's iterations when its Options say 0
//
// The lines (module of this namespace), what kvasir_test.py reads beside the run state:
//   TEST|START|seq=<request seq, 0 without one>|n=<selected>|of=<tests in the image>
//   TEST|RESET|<i>|<suite>.<name>|expecting <outcome>                 before a test that must end the run
//   TEST|PASS|<i>|<suite>.<name>|<us>us
//   TEST|FAIL|<i>|<suite>.<name>|<failures> failed|<us>us               (and TEST|EXPECT|<i>|... lines before it)
//   TEST|FAIL|<i>|<suite>.<name>|returned: expected <outcome>           a cross-reset test that returned
//   TEST|SKIP|<i>|<suite>.<name>                                        KVASIR_SKIP (TEST|SKIPPED|<i>|<reason> before)
//   TEST|SKIP|<i>|<suite>.<name>|disabled                               Options::enabled == false
//   TEST|PERF|<i>|<suite>.<name>|n=<samples>|min=<>|mean=<>|max=<>|<unit>
//   TEST|NOTE|<i>|no cycle counter: microseconds                        DwtCycles could not start
//   TEST|AFTER|<i>|<suite>.<name>|<verdict>|<reason>|panic=..|cause=..|pc=..|fault=..|watchdog=..
//   TEST|DONE|pass=<>|fail=<>|skip=<>|boots=<>|seq=<request seq>
#include "kvasir/Runtime/AnnouncedReset.hpp"
#include "kvasir/Test/Perf.hpp"
#include "kvasir/Test/RunState.hpp"
#include "kvasir/Test/Test.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Kvasir::Test {
/// No watchdog: an Outcome::watchdog test does not compile.
struct NoWatchdog {
    static constexpr bool          present   = false;
    static constexpr std::uint32_t timeoutMs = 0;

    static void arm() {}

    static void disarm() {}
};

/// The chip's watchdog (chip_rp_common Kvasir::Watchdog<Config>, the SAM/STM32 ones alike): its timeout is its
/// compile-time config. Give it pauseOnDebug = false in a test image: with a printer reading, the RP watchdogs pause
/// while the debugger accesses the bus.
template<typename W>
struct ChipWatchdog {
    static constexpr bool          present   = true;
    static constexpr std::uint32_t timeoutMs = static_cast<std::uint32_t>(
      std::chrono::ceil<std::chrono::milliseconds>(W::timeout).count());

    static void arm() { W::arm(); }

    static void disarm() { W::disarm(); }
};

struct DefaultConfig {
    using Watchdog = NoWatchdog;
    using Cycles   = void;
    static constexpr std::chrono::milliseconds hostWait{3000};
    static constexpr unsigned                  perfIterations = 100;
};

namespace Detail {
    template<typename Config>
    using CyclesOf = std::conditional_t<std::is_void_v<typename Config::Cycles>,
                                        ClockCycles<typename Config::Clock>,
                                        typename Config::Cycles>;

    inline std::uint32_t noClock() { return 0; }

    [[nodiscard]] constexpr std::uint16_t selectedCount(RunState const& s) {
        unsigned n = 0;
        for(auto const w : s.selected) { n += static_cast<unsigned>(std::popcount(w)); }
        return static_cast<std::uint16_t>(n);
    }

    template<typename Config, typename Flat>
    struct Engine;

    template<typename Config, typename... Ts>
    struct Engine<Config, List<Ts...>> {
        using Clock    = typename Config::Clock;
        using Watchdog = typename Config::Watchdog;
        using Cycles   = CyclesOf<Config>;
        using Infos    = Table<List<Ts...>>;

        static constexpr unsigned count = sizeof...(Ts);
        static_assert(count <= maxTests,
                      "Kvasir::Test: more tests in one image than the run state holds (maxTests)");

        template<unsigned I>
        using Nth = std::tuple_element_t<I, std::tuple<Ts...>>;

        template<unsigned I>
        static constexpr auto label = Nth<I>::suite + SC_LIFT(".") + Nth<I>::name;

        static void store(RunState const& s) { kvasir_test_run.store(s); }

        template<typename Since>
        [[nodiscard]] static std::uint32_t elapsedUs(Since t0) {
            return static_cast<std::uint32_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        }

        // A perf body: the clock started (or its fallback), the samples logged, the budget judged.
        template<unsigned I>
        static Result runPerf() {
            using T            = Nth<I>;
            constexpr auto o   = T::options;
            constexpr auto n   = o.iterations != 0 ? o.iterations : Config::perfIterations;
            bool const     dwt = Cycles::start();
            PerfState      perf{n, dwt ? &Cycles::now : &ClockCycles<Clock>::now};
            if(!dwt) { UC_LOG_W("TEST|NOTE|{}|no cycle counter: microseconds", I); }
            auto r = Slot<I, T>::run(perf);
            if(dwt) {
                UC_LOG_I(SC_LIFT("TEST|PERF|{}|") + label<I>
                           + SC_LIFT("|n={}|min={}|mean={}|max={}|") + Cycles::unit,
                         I,
                         perf.samples(),
                         perf.min(),
                         perf.mean(),
                         perf.max());
            } else {
                UC_LOG_I(SC_LIFT("TEST|PERF|{}|") + label<I>
                           + SC_LIFT("|n={}|min={}|mean={}|max={}|us"),
                         I,
                         perf.samples(),
                         perf.min(),
                         perf.mean(),
                         perf.max());
            }
            if constexpr(o.maxMean != 0) {
                if(dwt && perf.mean() > o.maxMean) {
                    ++r.failures;
                    UC_LOG_E("TEST|EXPECT|{}|mean {} over the budget {}",
                             I,
                             perf.mean(),
                             o.maxMean);
                }
            }
            return r;
        }

        template<unsigned I>
        static Result runBody() {
            if constexpr(Nth<I>::isPerf) {
                return runPerf<I>();
            } else {
                PerfState none{0, &noClock};
                return Slot<I, Nth<I>>::run(none);
            }
        }

        /// One test: in flight in the stored state while it runs (a crash or reset is judged at the next boot).
        template<unsigned I>
        static void runOne(RunState& s) {
            using T          = Nth<I>;
            constexpr auto o = T::options;
            s.next           = static_cast<std::uint16_t>(I + 1);
            if constexpr(!o.enabled) {
                // its slot stays in the image (a call the compiler cannot drop): the host lists the image's tests from
                // the slots, a disabled one too
                if(opaque(false)) {
                    PerfState none{0, &noClock};
                    (void)Slot<I, T>::run(none);
                }
                conclude(s, I, Verdict::skip);
                store(s);
                UC_LOG_I(SC_LIFT("TEST|SKIP|{}|") + label<I> + SC_LIFT("|disabled"), I);
            } else {
                constexpr bool crossReset = o.outcome != Outcome::returns;
                static_assert(o.outcome != Outcome::watchdog || Watchdog::present,
                              "Kvasir::Test: an Outcome::watchdog test needs Config::Watchdog "
                              "(ChipWatchdog<W>)");
                if(AnnouncedReset::printerListening()) {
                    s.flags = static_cast<std::uint8_t>(s.flags | RunState::printerListening);
                } else {
                    s.flags = static_cast<std::uint8_t>(s.flags & ~RunState::printerListening);
                }
                s.inFlight = static_cast<std::uint16_t>(I + 1);
                store(s);
                if constexpr(crossReset) {
                    UC_LOG_I(SC_LIFT("TEST|RESET|{}|") + label<I> + SC_LIFT("|expecting {}"),
                             I,
                             o.outcome);
                    if constexpr(o.outcome == Outcome::watchdog) {
                        // the printer stays away until the watchdog has bitten and the chip is up again
                        (void)AnnouncedReset::announce(Watchdog::timeoutMs + 1500U);
                        Watchdog::arm();
                    }
                }
                auto const t0 = Clock::now();
                auto const r  = runBody<I>();
                auto const us = elapsedUs(t0);
                if constexpr(o.outcome == Outcome::watchdog) { Watchdog::disarm(); }
                Verdict v = r.skipped       ? Verdict::skip
                          : r.failures != 0 ? Verdict::fail
                                            : Verdict::pass;
                if constexpr(crossReset) {
                    v = Verdict::fail;
                    UC_LOG_E(SC_LIFT("TEST|FAIL|{}|") + label<I>
                               + SC_LIFT("|returned: expected {}"),
                             I,
                             o.outcome);
                } else {
                    switch(v) {
                    case Verdict::pass:
                        UC_LOG_I(SC_LIFT("TEST|PASS|{}|") + label<I> + SC_LIFT("|{}us"), I, us);
                        break;
                    case Verdict::skip: UC_LOG_I(SC_LIFT("TEST|SKIP|{}|") + label<I>, I); break;
                    case Verdict::fail:
                    case Verdict::none:
                        UC_LOG_E(SC_LIFT("TEST|FAIL|{}|") + label<I> + SC_LIFT("|{} failed|{}us"),
                                 I,
                                 r.failures,
                                 us);
                        break;
                    }
                }
                conclude(s, I, v);
                store(s);
            }
        }

        /// The boot after a test in flight: its verdict (already counted by atBoot), then its second half.
        template<unsigned I>
        static void after(RunState&       s,
                          Judged          j,
                          Evidence const& e) {
            using T = Nth<I>;
            UC_LOG_I(SC_LIFT("TEST|AFTER|{}|") + label<I>
                       + SC_LIFT("|{}|{}|panic={}|cause={}|pc={:#x}|fault={}|watchdog={}"),
                     I,
                     j.verdict,
                     j.reason,
                     e.panic,
                     e.cause,
                     e.pc,
                     e.fault,
                     e.watchdog);
            if constexpr(hasAfterReset<T>) {
                Context c{static_cast<std::uint16_t>(I)};
                AfterReset<T>::run(c, e);
                if(c.failures() != 0 && verdictOf(s, I) == Verdict::pass) {
                    --s.pass;
                    ++s.fail;
                    setVerdict(s, I, Verdict::fail);
                    UC_LOG_E(SC_LIFT("TEST|FAIL|{}|") + label<I>
                               + SC_LIFT("|after reset: {} failed"),
                             I,
                             c.failures());
                }
            }
            store(s);
        }

        using RunFn   = void (*)(RunState&);
        using AfterFn = void (*)(RunState&,
                                 Judged,
                                 Evidence const&);

        template<std::size_t... Is>
        static constexpr std::array<RunFn,
                                    count>
        runTableOf(std::index_sequence<Is...>) {
            return {&runOne<Is>...};
        }

        template<std::size_t... Is>
        static constexpr std::array<AfterFn,
                                    count>
        afterTableOf(std::index_sequence<Is...>) {
            return {&after<Is>...};
        }

        static constexpr auto runTable   = runTableOf(std::make_index_sequence<count>{});
        static constexpr auto afterTable = afterTableOf(std::make_index_sequence<count>{});

        static void logStart(RunState const& s) {
            UC_LOG_I("TEST|START|seq={}|n={}|of={}", servedSeq(s), selectedCount(s), count);
        }

        static void logDone(RunState const& s) {
            UC_LOG_I("TEST|DONE|pass={}|fail={}|skip={}|boots={}|seq={}",
                     s.pass,
                     s.fail,
                     s.skip,
                     s.boots,
                     servedSeq(s));
        }

        /// The run from s.next to its end (or its first failure, if the request asked so), then DONE.
        static void runFrom(RunState& s) {
            for(unsigned i = nextSelected(s, s.next); i < count; i = nextSelected(s, s.next)) {
                runTable[i](s);
                if(stopsAfter(s, verdictOf(s, i))) { break; }
            }
            s.phase    = static_cast<std::uint8_t>(Phase::done);
            s.inFlight = 0;
            store(s);
            logDone(s);
        }
    };
}   // namespace Detail
}   // namespace Kvasir::Test
