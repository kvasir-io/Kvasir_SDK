#pragma once
// A test image's main: Kvasir::Test::Runner<Config, Tests>::run() - runs the listed tests (Test/Test.hpp) across the
// resets the cross-reset ones make, judges each from the evidence the next boot finds, and never returns.
//
//     struct TestConfig : Kvasir::Test::DefaultConfig {
//         using Clock    = HW::SystickClock;
//         using Watchdog = Kvasir::Test::ChipWatchdog<TestWatchdog>;   // for Outcome::watchdog tests
//         using Cycles   = Kvasir::Test::CoreCycles<Clock>;            // perf tests: DWT cycles where the core has them
//         using Startup  = ::Startup;                                  // optional: its MainLoop hooks run while idle
//     };
//     using Startup = Kvasir::Startup::Startup<HW::ClockSettings, Clock, HW::ComBackend, Kvasir::Boot::ReportInMain,
//                                              FaultHandler, StackProtector>;
//     int main() { Kvasir::Test::Runner<TestConfig, Tests>::run(); }
//
// The Startup list names Boot::ReportInMain: the runner reports the boot itself, after it took the panic and fault
// records as its evidence. A crashing test needs nothing of the image's own: the default panic reaction
// (Panic::OnPanic::Default) records and reboots the chip, a fault is a panic "fault" under Fault::HandlerFor's default
// (Fault::OnFault::Panic) - both announced to a listening printer.
//
// A boot, in this order:
//   1. the evidence: the panic and fault records (taken), the reset kind (PM::resetKind()); then the boot line
//      (Boot::logBoot with the records)
//   2. the run state (kvasir_test_run, Test/RunState.hpp) and the request decide (Detail::atBoot): a new request
//      starts a new run (TEST|READY, then TEST|START); a run in progress goes on, its test in flight judged now
//      (TEST|AFTER); else TEST|READY (with the image's build id and its host wait: logReady) and,
//      if a printer listens, up to Config::hostWait for a request - none: run every test (an image without a host
//      judges itself; the verdicts stay in the run state)
//   3. the tests from the state's next one on (Detail::Engine), each stored as in flight before it runs
//   4. TEST|DONE, then idle: the Startup's MainLoop hooks (Config::Startup), DONE again every 5 s, and a new request
//      starts a new run in this boot (READY first).
#include "kvasir/Port/Check.hpp"
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/ResetKind.hpp"
#include "kvasir/Runtime/AnnouncedReset.hpp"
#include "kvasir/Runtime/BootLog.hpp"
#include "kvasir/Test/Engine.hpp"
#include "uc_log/detail/BuildId.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace Kvasir::Test {
template<typename Config, typename Tests>
struct Runner {
    using Flat   = typename Detail::Flatten<Tests>::type;
    using Engine = Detail::Engine<Config, Flat>;
    using Clock  = typename Config::Clock;

    static constexpr unsigned count = Engine::count;

    /// What the boot found: the records the run before left, and whether the watchdog reset the chip.
    template<typename... Dep>
    static Evidence takeEvidence() {
        using Cause         = Port::LateT<Port::ResetCause<Tag::User>, Config, Dep...>;
        auto const cause    = Cause{}();
        auto const previous = Boot::takePrevious();
        Boot::logBoot(cause, previous);
        Evidence e{};
        if(previous.panic) {
            e.panic  = true;
            e.cause  = static_cast<Panic::Cause>(previous.panic->cause);
            e.pc     = previous.panic->pc;
            e.detail = previous.panic->detail;
        }
        e.fault         = previous.fault.has_value();
        auto const kind = PM::resetKind();
        static_assert(Port::ChipResetKind<decltype(PM::resetKind())>,
                      "Kvasir port: the chip package provides no PM::resetKind() (its PM.hpp / "
                      "chip_reset.hpp)");
        e.watchdog = kind == ResetKind::watchdogTimeout || kind == ResetKind::watchdogForced;
        return e;
    }

    /// The image's build id: the first 8 bytes of its GNU build-id note (uc_log's `uc_log_build_id_note`, the note the
    /// log printer's firmware check compares), as two little-endian words; 0 for an image linked without the note.
    [[nodiscard]] static std::array<std::uint32_t,
                                    2>
    buildId() {
        if(&uc_log_build_id_note == nullptr) { return {}; }
        return {uc_log_build_id_note.hash[0], uc_log_build_id_note.hash[1]};
    }

    /// TEST|READY|n=<tests>|seq=<last request>|build=<the build id, 16 hex digits>|wait=<hostWait>ms: what the host
    /// checks the board's image by (against the note in its ELF: no flash compare) and how long the image waits.
    static void logReady(RunState const& s) {
        auto const id = buildId();
        UC_LOG_I(
          "TEST|READY|n={}|seq={}|build={:08x}{:08x}|wait={}ms",
          count,
          s.requestSeq,
          id[0],
          id[1],
          static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Config::hostWait).count()));
    }

    /// No run to go on with: READY, then the host's request if one comes within hostWait - else everything.
    static RunState waitForHost(RunState const& s) {
        logReady(s);
        if(AnnouncedReset::printerListening() || AnnouncedReset::waitForPrinter(3'000'000)) {
            auto const until = Clock::now() + Config::hostWait;
            while(Clock::now() < until) {
                auto const r = Detail::copy(kvasir_test_request);
                if(Detail::requestIsNew(r, s)) { return Detail::freshRun(count, 0, &r); }
            }
        }
        return Detail::freshRun(count, s.requestSeq, nullptr);
    }

    [[noreturn]] static void run() {
        auto const evidence = takeEvidence();
        auto const request  = Detail::copy(kvasir_test_request);
        auto d = Detail::atBoot(kvasir_test_run.load(), request, evidence, Engine::Infos::infos);
        RunState s = d.state;
        switch(d.start) {
        case Detail::Start::resumed:
            if(d.judged) { Engine::afterTable[d.judgedIndex](s, *d.judged, evidence); }
            // a printer that heard the reset is away for its pause: the next reset must not fall into its reconnect
            if((s.flags & RunState::printerListening) != 0) {
                (void)AnnouncedReset::waitForPrinter(3'000'000);
            }
            break;
        case Detail::Start::noRun:
            s = waitForHost(s);
            Engine::logStart(s);
            break;
        case Detail::Start::fresh:
            logReady(s);
            Engine::logStart(s);
            break;
        }
        Engine::store(s);
        Engine::runFrom(s);
        idle(s);
    }

private:
    [[noreturn]] static void idle(RunState& s) {
        auto next = Clock::now() + std::chrono::seconds{5};
        while(true) {
            if constexpr(requires { typename Config::Startup; }) {
                Config::Startup::template run<Hook::MainLoop>();
            }
            auto const r = Detail::copy(kvasir_test_request);
            if(Detail::requestIsNew(r, s)) {
                s = Detail::freshRun(count, 0, &r);
                logReady(s);   // the host checks the image by it in a run without a flash too
                Engine::logStart(s);
                Engine::store(s);
                Engine::runFrom(s);
                next = Clock::now() + std::chrono::seconds{5};
            }
            if(Clock::now() >= next) {
                Engine::logDone(s);
                next = next + std::chrono::seconds{5};
            }
        }
    }
};
}   // namespace Kvasir::Test
