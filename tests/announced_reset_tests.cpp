// Tests for Kvasir::AnnouncedReset: the firmware's side of the reset handshake with the log
// printer (uc_log JLinkRttReader). The printer is played by a scripted access: it answers the ack
// after some polls, or never.
#include "kvasir/Runtime/AnnouncedReset.hpp"
#include "test_harness.hpp"

// what a chip package's chip/WaitBounds.hpp does: the fastest clock the polls are counted at, after the header (the
// lookup is late)
template<>
struct Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User> {
    static constexpr std::uint64_t value = 266'000'000;
};

// what an application binds (AnnouncedReset::WatchdogFor, next to its watchdog's alias), here a watchdog that counts
// its feeds and how many polls of a wait went by since the last one
namespace announced_reset_test {
struct FeedLog {
    std::uint64_t feeds{};
    std::uint32_t pollsSinceFeed{};
    std::uint32_t maxGap{};   // the most polls of a wait between two feeds
};

inline FeedLog feedLog{};

// what Detail::feedFor asks of the binding
template<typename Hook>
concept Bound = requires { typename Hook::type; };

struct FakeWatchdog {
    static void feed(Kvasir::Health::AnnouncedResetKey /*key*/) {
        ++feedLog.feeds;
        feedLog.pollsSinceFeed = 0;
    }
};
}   // namespace announced_reset_test

template<>
struct Kvasir::AnnouncedReset::WatchdogFor<Kvasir::Tag::User> {
    using type = announced_reset_test::FakeWatchdog;
};

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>

#if defined(                                                                                 \
  KVASIR_TEST_UC_LOG_PRINTER)   // tests/CMakeLists.txt, where the nested uc_log checkout is
    #include "uc_log/printer/AnnouncedReset.hpp"
// the printer's side must use the same numbers
static_assert(Kvasir::AnnouncedReset::magicValue == uc_log::printer::announced_reset::magicValue);
static_assert(Kvasir::AnnouncedReset::hostArmed == uc_log::printer::announced_reset::hostArmed);
#endif

namespace {
using namespace Kvasir::AnnouncedReset;

// The block as words, the printer's answer scripted: `ackAfter` polls of ack after the request
// it writes ack = request (nullopt: never).
struct ScriptedAccess {
    mutable std::array<std::uint32_t, 4> words{magicValue, hostArmed, 0, 0};
    std::optional<std::uint32_t>         ackAfter{};
    mutable std::uint32_t                ackPolls{};
    mutable std::uint32_t                gracePolls{};
    mutable std::uint32_t                requestStores{};
    mutable bool                         acked{};

    std::uint32_t load(Field f) const {
        auto const i = static_cast<std::size_t>(f);
        if(f == Field::ack
           || (f == Field::magic && acked)) {   // a poll of the ack wait or of the grace
            auto& log = announced_reset_test::feedLog;
            ++log.pollsSinceFeed;
            log.maxGap = log.pollsSinceFeed > log.maxGap ? log.pollsSinceFeed : log.maxGap;
        }
        if(f == Field::ack) {
            ++ackPolls;
            if(ackAfter && ackPolls > *ackAfter) {
                words[3] = words[2];
                acked    = true;
            }
        }
        if(f == Field::magic && acked) { ++gracePolls; }
        return words[i];
    }

    void store(Field         f,
               std::uint32_t v) const {
        if(f == Field::request) { ++requestStores; }
        words[static_cast<std::size_t>(f)] = v;
    }
};

constexpr Polls small{100, 7};

// a microsecond counter that advances `step` a read (0: stands still)
std::uint32_t clockNow{};
std::uint32_t clockStep{};

std::uint32_t fakeClock() {
    clockNow += clockStep;
    return clockNow;
}
}   // namespace

int main() {
    Kvasir::Test::test("no debugger: at once, nothing written");
    {
        ScriptedAccess const a{.ackAfter = 0};
        CHECK(announceVia(a, false, small) == Outcome::notArmed);
        CHECK(a.requestStores == 0 && a.ackPolls == 0);
    }

    Kvasir::Test::test("not armed by a printer: at once");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[1] = 0;
        CHECK(announceVia(a, true, small) == Outcome::notArmed);
        CHECK(a.requestStores == 0 && a.ackPolls == 0);
    }

    Kvasir::Test::test("no magic (not the block): at once");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[0] = 0;
        CHECK(announceVia(a, true, small) == Outcome::notArmed);
        CHECK(a.requestStores == 0);
    }

    Kvasir::Test::test("acknowledged: request bumped, waits for the ack, then the grace");
    {
        ScriptedAccess a{.ackAfter = 10};
        a.words[2] = 4;
        a.words[3] = 4;   // the previous announce of this boot was acked
        CHECK(announceVia(a, true, small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 5U);
        CHECK_EQ(a.words[3], 5U);
        CHECK_EQ(a.ackPolls, 11U);
        CHECK_EQ(a.gracePolls, small.grace);
    }

    Kvasir::Test::test("no answer: gives up after the bound");
    {
        ScriptedAccess a{};
        CHECK(announceVia(a, true, small) == Outcome::timedOut);
        CHECK_EQ(a.words[2], 1U);
        CHECK_EQ(a.ackPolls, small.ack);
        CHECK_EQ(a.gracePolls, 0U);
    }

    Kvasir::Test::test("the request never wraps to 0 (0 is a fresh boot's)");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[2] = 0xFFFF'FFFFU;
        CHECK(announceVia(a, true, small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 1U);
    }

    Kvasir::Test::test("a stay-away time rides in the request's upper half, the sequence goes on");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[2] = (7U << 16) | 41U;   // an earlier request that asked for 0.7 s
        CHECK(announceVia(a, true, small, 9'350) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], (94U << 16) | 42U);   // 9.35 s rounded up to 9.4 s, sequence + 1
        CHECK(announceVia(a, true, small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 43U);   // no time asked: the printer's default
        a.words[2] = 0xFFFFU;
        CHECK(announceVia(a, true, small, 10'000'000) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], (0xFFFFU << 16) | 1U);   // clamped to 6553.5 s, sequence never 0
    }

    Kvasir::Test::test("with a running microsecond counter the waits are timed, not counted");
    {
        clockNow  = 0xFFFF'FF00U;   // across the counter's wrap
        clockStep = 10;
        ScriptedAccess const a{.ackAfter = 30};
        // 50 polls of counted grace, but 200 us of time at 10 us a poll: ~20 polls
        CHECK(announceVia(a, true, Polls{100'000, 50}, 0, &fakeClock, Waits{1'000'000, 200})
              == Outcome::acknowledged);
        CHECK(a.gracePolls >= 19 && a.gracePolls <= 21);
        ScriptedAccess const none{};
        // no answer: 1000 us at 10 us a read, long before the 100'000 counted polls
        CHECK(announceVia(none, true, Polls{100'000, 50}, 0, &fakeClock, Waits{1'000, 200})
              == Outcome::timedOut);
        CHECK(none.ackPolls >= 99 && none.ackPolls <= 101);
    }

    Kvasir::Test::test("a counter that stands still: counted polls after StillPolls");
    {
        clockStep = 0;
        ScriptedAccess const a{};
        Polls const          counted{Detail::pollsFor<>(Detail::stillUs) + 50, 5};
        CHECK(announceVia(a, true, counted, 0, &fakeClock, Waits{1, 1}) == Outcome::timedOut);
        CHECK_EQ(a.ackPolls, counted.ack);
    }

    Kvasir::Test::test("on the real (volatile) block");
    {
        Block volatile block{magicValue, hostArmed, 0, 0};
        CHECK(announceOn(block, true, small) == Outcome::timedOut);
        CHECK_EQ(block.request, 1U);
        block.ack = 2;   // a printer that already answers the next one
        CHECK(announceOn(block, true, small) == Outcome::acknowledged);
        CHECK_EQ(block.request, 2U);
        CHECK(announceOn(block, false, small) == Outcome::notArmed);
        CHECK_EQ(block.request, 2U);
    }

    Kvasir::Test::test("the firmware's block starts unarmed, request == ack");
    {
        CHECK_EQ(kvasir_announced_reset.magic, magicValue);
        CHECK_EQ(kvasir_announced_reset.armed, 0U);
        CHECK_EQ(kvasir_announced_reset.request, kvasir_announced_reset.ack);
        CHECK(announce() == Outcome::notArmed);   // no printer armed it
    }

    Kvasir::Test::test(
      "waitForPrinterOn: false when nobody arms the block, true once a printer has");
    {
        Block volatile block{magicValue, 0, 0, 0};
        CHECK(!printerListeningOn(block));
        CHECK(!waitForPrinterOn(block, 10));   // a short bound: nobody arms it here
        block.armed = hostArmed;
        CHECK(printerListeningOn(block));
        CHECK(waitForPrinterOn(block, 10));
    }

#ifndef USE_UC_LOG
    Kvasir::Test::test(
      "without USE_UC_LOG the public calls are constants, even with the block armed");
    {
        announced_reset_test::feedLog = {};
        kvasir_announced_reset.armed  = hostArmed;
        CHECK(announce() == Outcome::notArmed);
        CHECK(announce(4'000) == Outcome::notArmed);
        CHECK(!printerListening());
        CHECK(!waitForPrinter(10));
        CHECK_EQ(kvasir_announced_reset.request, 0U);   // untouched
        CHECK_EQ(announced_reset_test::feedLog.feeds,
                 std::uint64_t{0});   // and the watchdog not fed
        kvasir_announced_reset.armed = 0;
    }
#endif

    Kvasir::Test::test("the bounds: >= 0.5 s / 0.1 s at the chip's fastest clock (CpuHzCeiling)");
    {
        using namespace Kvasir::AnnouncedReset::Detail;
        constexpr Polls p = defaultPolls<>();
        static_assert(maxCoreHz<>() == 266'000'000);
        CHECK(static_cast<std::uint64_t>(p.ack) * minCyclesPerPoll
              >= maxCoreHz<>() / 2 - minCyclesPerPoll);
        CHECK(static_cast<std::uint64_t>(p.grace) * minCyclesPerPoll
              >= maxCoreHz<>() / 10 - minCyclesPerPoll);
        // the 200 MHz the bound once assumed would wait ~0.375 s on a 266 MHz part
        CHECK(static_cast<std::uint64_t>(p.ack) * minCyclesPerPoll > 200'000'000ULL / 2);
    }

    using announced_reset_test::feedLog;

    Kvasir::Test::test("the bound watchdog is fed at every poll of the ack wait and of the grace");
    {
        feedLog = {};
        ScriptedAccess a{.ackAfter = 10};
        CHECK(announceVia(a, true, small) == Outcome::acknowledged);
        CHECK_EQ(a.ackPolls, 11U);
        CHECK_EQ(a.gracePolls, small.grace);
        CHECK_EQ(feedLog.feeds, std::uint64_t{a.ackPolls + a.gracePolls});
        CHECK_EQ(feedLog.maxGap, 1U);   // never two polls without a feed between them
    }

    Kvasir::Test::test("no answer: fed to the end of the bound");
    {
        feedLog = {};
        ScriptedAccess a{};
        CHECK(announceVia(a, true, small) == Outcome::timedOut);
        CHECK_EQ(a.ackPolls, small.ack);
        CHECK_EQ(feedLog.feeds, std::uint64_t{small.ack});
        CHECK_EQ(feedLog.maxGap, 1U);
    }

    Kvasir::Test::test("not armed, no debugger: nothing fed (the caller's watchdog as it was)");
    {
        feedLog = {};
        ScriptedAccess a{.ackAfter = 0};
        a.words[1] = 0;
        CHECK(announceVia(a, true, small) == Outcome::notArmed);
        CHECK(announceVia(a, false, small) == Outcome::notArmed);
        CHECK_EQ(feedLog.feeds, std::uint64_t{0});
    }

    // the binding is looked up where the image calls, not where the header is read: a call of the header on a block
    // (announceOn) and the application's own call with the header's access feed it too
    Kvasir::Test::test("announceOn() and announceVia(VolatileAccess) feed the bound watchdog");
    {
        Block volatile silent{magicValue,
                              hostArmed,
                              0,
                              0};   // a printer armed it and died: no ack ever
        feedLog = {};
        CHECK(announceOn(silent, true, small) == Outcome::timedOut);
        CHECK_EQ(feedLog.feeds, std::uint64_t{small.ack});
        feedLog = {};
        CHECK(announceVia(VolatileAccess{silent}, true, small) == Outcome::timedOut);
        CHECK_EQ(feedLog.feeds, std::uint64_t{small.ack});
    }

    Kvasir::Test::test("an image that binds no watchdog feeds nothing");
    {
        // the binding is what FeedOf finds; without one (the primary, declared only) there is no type to find
        static_assert(std::is_same_v<Detail::FeedOf<>,
                                     Detail::FeedWatchdog<announced_reset_test::FakeWatchdog>>);
        static_assert(!announced_reset_test::Bound<WatchdogFor<struct NoImage>>);
        feedLog = {};
        ScriptedAccess a{.ackAfter = 3};
        CHECK(
          (announceVia<ScriptedAccess, Detail::NoFeed>(a, true, small) == Outcome::acknowledged));
        CHECK_EQ(feedLog.feeds, std::uint64_t{0});
    }

#ifdef USE_UC_LOG
    Kvasir::Test::test(
      "announce() feeds the bound watchdog at every poll (the image's WatchdogFor)");
    {
        feedLog = {};
        kvasir_announced_reset.armed
          = hostArmed;   // a printer armed the block and is gone: no ack comes
        CHECK(announce() == Outcome::timedOut);
        CHECK_EQ(feedLog.feeds, std::uint64_t{defaultPolls<>().ack});
        kvasir_announced_reset.armed = 0;
        kvasir_announced_reset.ack   = kvasir_announced_reset.request;
    }
#endif

    return Kvasir::Test::report();
}
