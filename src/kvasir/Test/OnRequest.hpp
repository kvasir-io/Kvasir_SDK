#pragma once
// A factory self-test inside an application: Kvasir::Test::OnRequest<Config, Tests> in the Startup list runs the listed
// tests (Test/Test.hpp) from the main loop when a host asks (kvasir_test.py run --no-flash against the running
// product), with the lines and the run state a test image has (Test/Engine.hpp, Test/RunState.hpp).
//
//     struct SelfTestConfig : Kvasir::Test::DefaultConfig { using Clock = HW::SystickClock; };
//     using SelfTests = Kvasir::Test::List<KvasirTests::sensors::Suite>;
//     using Startup = Kvasir::Startup::Startup<..., Kvasir::Test::OnRequest<SelfTestConfig, SelfTests>, ...>;
//
// The tests must return (Outcome::returns): an application's panics and resets are its own (its boot report takes
// the records). A test runs inside one main-loop turn: the loop stands while it runs. A reset during a test (the
// product's watchdog, a crash) leaves it in flight: the next boot's runtimeInit marks it FAIL (reason reset) and ends
// the run. Cost: the tests' code, the run state (124 B .noInit) and the request (44 B .noInit), the turn's two loads.
// An image that does not list it has none of it (nothing here is in any other header).
#include "kvasir/Port/Extends.hpp"
#include "kvasir/Test/Engine.hpp"

#include <cstdint>

namespace Kvasir::Test {
template<typename Config, typename Tests>
struct OnRequest {
    using Flat   = typename Detail::Flatten<Tests>::type;
    using Engine = Detail::Engine<Config, Flat>;

    static constexpr unsigned count = Engine::count;

    static_assert(
      [] {
          for(auto const& t : Engine::Infos::infos) {
              if(t.outcome != Outcome::returns) { return false; }
          }
          return true;
      }(),
      "Kvasir::Test::OnRequest: every test of an application's self-test returns "
      "(Outcome::returns) - a test that "
      "ends the run belongs in a test image (Test/Runner.hpp)");

    /// The request the run state took last: the turn compares it with two loads.
    static inline std::uint32_t taken{};

    /// A run a reset cut short: its test in flight failed, the run ends.
    static void runtimeInit() {
        auto const loaded = kvasir_test_run.load();
        if(!loaded) { return; }
        RunState s = *loaded;
        taken      = s.requestSeq;
        if(s.phase != static_cast<std::uint8_t>(Phase::running) || s.count != count) { return; }
        ++s.boots;
        if(s.inFlight != 0 && s.inFlight <= count) {
            auto const i = static_cast<unsigned>(s.inFlight - 1U);
            Detail::conclude(s, i, Verdict::fail);
            Engine::afterTable[i](s, Detail::Judged{Verdict::fail, Reason::reset}, Evidence{});
        }
        s.phase = static_cast<std::uint8_t>(Phase::done);
        Engine::store(s);
        Engine::logDone(s);
    }

    /// The main loop's turn: a new request runs its tests now.
    static Turn turn() {
        if(kvasir_test_request.magic != Request::magicValue || kvasir_test_request.seq == taken
           || kvasir_test_request.seq == 0)
        {
            return Turn::idle();
        }
        auto const r = Detail::copy(kvasir_test_request);
        taken        = r.seq;
        RunState s   = Detail::freshRun(count, 0, &r);
        Engine::logStart(s);
        Engine::store(s);
        Engine::runFrom(s);
        return Turn::idle();
    }

    using Extends = Startup::Extend<Hook::MainLoop, &OnRequest::turn>;
};
}   // namespace Kvasir::Test
