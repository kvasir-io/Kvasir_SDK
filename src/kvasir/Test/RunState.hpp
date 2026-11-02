#pragma once
// The on-device test run as data: the host's request, the run state that survives the resets of cross-reset tests and
// crashes, and the pure decisions over them (host-tested: tests/device_test_framework_tests.cpp). The runner
// (Test/Runner.hpp) and OnRequest (Test/OnRequest.hpp) do the hardware half.
//
// The two objects have C names (kvasir_test.py finds them in the image's symbols, as kvasir_announced_reset), both in
// .noInit:
//   kvasir_test_request  written by the host through the log printer's control socket (`write`), in this order:
//                        word 0 (the magic) cleared, words 1.. (seq, flags, selection), the magic - so the runner never
//                        takes half a request, nor a new seq with the last request's selection (the magic of the
//                        last one is still there). A request is new when its seq differs from the last one taken.
//   kvasir_test_run      a Persistent<RunState, RunTag> ("KTST", CRC-checked: garbage after a power-up is "no run").
//                        Layout for the host: word 0 the id, words 1..sizeof(RunState)/4 the RunState below, the
//                        last word the CRC-32 (zlib) over the words before it. The verdicts here are authoritative,
//                        the log is not (RTT in skip mode drops lines).
#include "kvasir/Test/Test.hpp"
#include "kvasir/Util/Persistent.hpp"
#include "kvasir/Util/attributes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>

namespace Kvasir::Test {
/// The most tests one image lists (2 bits each in the run state, 1 bit in a request's selection).
inline constexpr unsigned maxTests = 256;

/// What the host asks for. Written by the host only.
struct Request {
    static constexpr std::uint32_t magicValue = persistentId("KTRQ");
    /// flags: stop the run at the first failed test.
    static constexpr std::uint32_t stopAtFirstFailure = 1U << 0U;

    std::uint32_t magic;
    std::uint32_t seq;   // != the run state's requestSeq: a new run; never 0
    std::uint32_t flags;
    // bit i: run test i. A plain array: the block is accessed volatile, std::array has no volatile operator[]
    std::uint32_t select[maxTests / 32];
};

static_assert(sizeof(Request) == 44,
              "kvasir_test.py writes the request as 11 words");

/// Where a run is.
enum class Phase : std::uint8_t { idle, running, done };

/// The run, across the reboots of its cross-reset tests. Read by the host at the end.
struct RunState {
    /// flags
    static constexpr std::uint8_t fromRequest
      = 1U << 0U;   // the run serves request requestSeq (else: run all)
    static constexpr std::uint8_t stopAtFirstFail = 1U << 1U;
    static constexpr std::uint8_t printerListening
      = 1U << 2U;   // a printer listened when the last test started

    std::uint32_t requestSeq;   // the last request taken (0: none yet)
    std::uint16_t next;         // the next test to start
    std::uint16_t inFlight;     // index + 1 of a test that started and did not end, 0: none
    std::uint16_t pass;
    std::uint16_t fail;
    std::uint16_t skip;
    std::uint16_t boots;   // boots this run took, the first one included
    std::uint8_t  phase;   // Phase
    std::uint8_t  flags;
    std::uint16_t count;   // tests in the image
    std::array<std::uint32_t, maxTests / 16>
      verdicts;   // 2 bits a test (Verdict), test i at bits 2i % 32
    std::array<std::uint32_t, maxTests / 32> selected;   // bit i: test i is part of the run
};

static_assert(sizeof(RunState) == 116,
              "kvasir_test.py reads the run state as 29 words");

struct RunTag {
    static constexpr std::uint32_t id = persistentId("KTST");
};

using RunRecord = Persistent<RunState, RunTag>;

/// What a test is to the run: whether it runs, and how it must end.
struct TestInfo {
    Outcome      outcome{};
    Panic::Cause cause{};
    bool         enabled{true};
};

/// Why a cross-reset test (or a crashed one) got its verdict at the next boot.
enum class Reason : std::uint8_t {
    asExpected,     // the evidence matches the outcome
    wrongCause,     // a panic, but with another cause
    noPanic,        // a panic was expected, none was recorded
    noFault,        // a fault was expected, none was recorded
    notWatchdog,    // the watchdog was expected; another reset (or a record) came
    crashedPanic,   // a test that should have returned panicked
    crashedFault,   // ... faulted
    timeout,        // ... was reset by the watchdog
    reset,          // ... was reset otherwise (a debugger, the reset pin, a reboot of its own)
    didNotEnd,      // a cross-reset test returned
};

namespace Detail {
    [[nodiscard]] constexpr Verdict verdictOf(RunState const& s,
                                              unsigned        i) {
        return static_cast<Verdict>((s.verdicts[i / 16] >> (2U * (i % 16))) & 3U);
    }

    constexpr void setVerdict(RunState& s,
                              unsigned  i,
                              Verdict   v) {
        auto& w = s.verdicts[i / 16];
        w = (w & ~(3U << (2U * (i % 16)))) | (static_cast<std::uint32_t>(v) << (2U * (i % 16)));
    }

    [[nodiscard]] constexpr bool isSelected(RunState const& s,
                                            unsigned        i) {
        return ((s.selected[i / 32] >> (i % 32)) & 1U) != 0;
    }

    /// Count a verdict and keep it.
    constexpr void conclude(RunState& s,
                            unsigned  i,
                            Verdict   v) {
        setVerdict(s, i, v);
        switch(v) {
        case Verdict::pass: ++s.pass; break;
        case Verdict::fail: ++s.fail; break;
        case Verdict::skip: ++s.skip; break;
        case Verdict::none: break;
        }
        s.inFlight = 0;
    }

    /// A fresh run over `count` tests: those `select` names (all, without a request), from the first boot.
    [[nodiscard]] constexpr RunState freshRun(std::uint16_t  count,
                                              std::uint32_t  lastSeq,
                                              Request const* request) {
        RunState s{};
        s.requestSeq = request != nullptr ? request->seq : lastSeq;
        s.phase      = static_cast<std::uint8_t>(Phase::running);
        s.count      = count;
        s.boots      = 1;
        if(request != nullptr) {
            s.flags = RunState::fromRequest;
            if((request->flags & Request::stopAtFirstFailure) != 0) {
                s.flags |= RunState::stopAtFirstFail;
            }
        }
        for(unsigned i = 0; i != count; ++i) {
            bool const on = request == nullptr || ((request->select[i / 32] >> (i % 32)) & 1U) != 0;
            if(on) { s.selected[i / 32] |= 1U << (i % 32); }
        }
        return s;
    }

    /// A request is new when it is complete (the magic, a seq) and the state has not taken it.
    [[nodiscard]] constexpr bool requestIsNew(Request const&                 r,
                                              std::optional<RunState> const& state) {
        return r.magic == Request::magicValue && r.seq != 0
            && (!state || state->requestSeq != r.seq);
    }

    struct Judged {
        Verdict verdict;
        Reason  reason;
    };

    /// The verdict of the test that was in flight when the chip reset, from what the boot found.
    [[nodiscard]] constexpr Judged judge(TestInfo const& t,
                                         Evidence const& e) {
        bool const faultSeen = e.fault || (e.panic && e.cause == Panic::Cause::fault);
        switch(t.outcome) {
        case Outcome::panic:
            if(!e.panic) { return {Verdict::fail, Reason::noPanic}; }
            return e.cause == t.cause ? Judged{Verdict::pass, Reason::asExpected}
                                      : Judged{Verdict::fail, Reason::wrongCause};
        case Outcome::fault:
            return faultSeen ? Judged{Verdict::pass, Reason::asExpected}
                             : Judged{Verdict::fail, Reason::noFault};
        case Outcome::watchdog:
            return e.watchdog && !e.panic && !e.fault ? Judged{Verdict::pass, Reason::asExpected}
                                                      : Judged{Verdict::fail, Reason::notWatchdog};
        case Outcome::reset:   return {Verdict::pass, Reason::asExpected};
        case Outcome::returns: break;
        }
        // a test that should have returned did not: what ended it
        if(faultSeen) { return {Verdict::fail, Reason::crashedFault}; }
        if(e.panic) { return {Verdict::fail, Reason::crashedPanic}; }
        if(e.watchdog) { return {Verdict::fail, Reason::timeout}; }
        return {Verdict::fail, Reason::reset};
    }

    /// What a boot of a test image does.
    enum class Start : std::uint8_t {
        fresh,     // a new request: a new run from test 0
        resumed,   // a run in progress goes on (after a cross-reset test or a crash)
        noRun,     // nothing to go on with: wait for the host, else run everything
    };

    struct BootDecision {
        RunState              state;
        Start                 start;
        std::optional<Judged> judged;   // the in-flight test's verdict (resumed only)
        std::uint16_t         judgedIndex;
    };

    /// The boot's decision, before any test runs: a new request wins over a run in progress (its in-flight test is
    /// dropped with it); a run in progress counts this boot and judges its in-flight test from the evidence. A request
    /// counts at boot only beside a valid run state: without one (a power-up, another image ran in between - its
    /// .noInit is not ours) a valid-looking request is a leftover, and taking it would start a run nobody waits for,
    /// whose resets fall into the host's own request write. The host then sees READY and asks.
    [[nodiscard]] constexpr BootDecision atBoot(std::optional<RunState> const& loaded,
                                                Request const&                 request,
                                                Evidence const&                evidence,
                                                std::span<TestInfo const>      tests) {
        auto const count = static_cast<std::uint16_t>(tests.size());
        if(loaded && requestIsNew(request, loaded)) {
            return {freshRun(count, 0, &request), Start::fresh, std::nullopt, 0};
        }
        if(loaded && loaded->phase == static_cast<std::uint8_t>(Phase::running)
           && loaded->count == count)
        {
            RunState s = *loaded;
            ++s.boots;
            if(s.inFlight != 0 && s.inFlight <= count) {
                auto const i = static_cast<std::uint16_t>(s.inFlight - 1);
                auto const j = judge(tests[i], evidence);
                conclude(s, i, j.verdict);
                if(s.next <= i) { s.next = static_cast<std::uint16_t>(i + 1); }
                return {s, Start::resumed, j, i};
            }
            s.inFlight = 0;
            return {s, Start::resumed, std::nullopt, 0};
        }
        RunState s{};
        if(loaded) { s = *loaded; }
        s.count = count;
        return {s, Start::noRun, std::nullopt, 0};
    }

    /// Whether the run ends after this verdict.
    [[nodiscard]] constexpr bool stopsAfter(RunState const& s,
                                            Verdict         v) {
        return v == Verdict::fail && (s.flags & RunState::stopAtFirstFail) != 0;
    }

    /// The next selected test from `from` on, or count.
    [[nodiscard]] constexpr unsigned nextSelected(RunState const& s,
                                                  unsigned        from) {
        while(from < s.count && !isSelected(s, from)) { ++from; }
        return from;
    }

    /// The DONE line's seq: the request this run served, 0 for a run without one.
    [[nodiscard]] constexpr std::uint32_t servedSeq(RunState const& s) {
        return (s.flags & RunState::fromRequest) != 0 ? s.requestSeq : 0U;
    }

    /// A copy of the request block, word by word (the host writes it through the probe).
    [[nodiscard]] inline Request copy(Request const volatile& r) {
        Request c{};
        c.magic = r.magic;
        c.seq   = r.seq;
        c.flags = r.flags;
        for(std::size_t i = 0; i != std::size(c.select); ++i) { c.select[i] = r.select[i]; }
        return c;
    }

    template<typename T>
    struct InfoOf {
        static constexpr TestInfo value{T::options.outcome, T::options.cause, T::options.enabled};
    };

    template<typename Flat>
    struct Table;

    template<typename... Ts>
    struct Table<List<Ts...>> {
        static constexpr unsigned                            count = sizeof...(Ts);
        static constexpr std::array<TestInfo, sizeof...(Ts)> infos{InfoOf<Ts>::value...};
    };
}   // namespace Detail
}   // namespace Kvasir::Test

extern "C" {
// The host's request and the run state (header comment). volatile request: written from outside.
[[KVASIR_NOINIT]] inline Kvasir::Test::Request volatile kvasir_test_request;
[[KVASIR_NOINIT]] inline Kvasir::Test::RunRecord kvasir_test_run;
}
