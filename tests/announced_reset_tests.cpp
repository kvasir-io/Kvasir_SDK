// Tests for Kvasir::AnnouncedReset: the firmware's side of the reset handshake with the log
// printer (uc_log JLinkRttReader). The printer is played by a scripted access: it answers the ack
// after some polls, or never.
#include "kvasir/Util/AnnouncedReset.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <optional>

#if __has_include("../uc_log/src/uc_log/detail/AnnouncedReset.hpp")
    #include "../uc_log/src/uc_log/detail/AnnouncedReset.hpp"
// the printer's side must use the same numbers
static_assert(Kvasir::AnnouncedReset::Magic == uc_log::detail::announced_reset::Magic);
static_assert(Kvasir::AnnouncedReset::HostArmed == uc_log::detail::announced_reset::HostArmed);
#endif

namespace {
using namespace Kvasir::AnnouncedReset;

// The block as words, the printer's answer scripted: `ackAfter` polls of ack after the request
// it writes ack = request (nullopt: never).
struct ScriptedAccess {
    mutable std::array<std::uint32_t, 4> words{Magic, HostArmed, 0, 0};
    std::optional<std::uint32_t>         ackAfter{};
    mutable std::uint32_t                ackPolls{};
    mutable std::uint32_t                gracePolls{};
    mutable std::uint32_t                requestStores{};
    mutable bool                         acked{};

    std::uint32_t load(Field f) const {
        auto const i = static_cast<std::size_t>(f);
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

constexpr Polls Small{100, 7};
}   // namespace

int main() {
    Kvasir::Test::test("no debugger: at once, nothing written");
    {
        ScriptedAccess a{.ackAfter = 0};
        CHECK(announceVia(a, false, Small) == Outcome::notArmed);
        CHECK(a.requestStores == 0 && a.ackPolls == 0);
    }

    Kvasir::Test::test("not armed by a printer: at once");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[1] = 0;
        CHECK(announceVia(a, true, Small) == Outcome::notArmed);
        CHECK(a.requestStores == 0 && a.ackPolls == 0);
    }

    Kvasir::Test::test("no magic (not the block): at once");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[0] = 0;
        CHECK(announceVia(a, true, Small) == Outcome::notArmed);
        CHECK(a.requestStores == 0);
    }

    Kvasir::Test::test("acknowledged: request bumped, waits for the ack, then the grace");
    {
        ScriptedAccess a{.ackAfter = 10};
        a.words[2] = 4;
        a.words[3] = 4;   // the previous announce of this boot was acked
        CHECK(announceVia(a, true, Small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 5U);
        CHECK_EQ(a.words[3], 5U);
        CHECK_EQ(a.ackPolls, 11U);
        CHECK_EQ(a.gracePolls, Small.grace);
    }

    Kvasir::Test::test("no answer: gives up after the bound");
    {
        ScriptedAccess a{};
        CHECK(announceVia(a, true, Small) == Outcome::timedOut);
        CHECK_EQ(a.words[2], 1U);
        CHECK_EQ(a.ackPolls, Small.ack);
        CHECK_EQ(a.gracePolls, 0U);
    }

    Kvasir::Test::test("the request never wraps to 0 (0 is a fresh boot's)");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[2] = 0xFFFF'FFFFU;
        CHECK(announceVia(a, true, Small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 1U);
    }

    Kvasir::Test::test("a stay-away time rides in the request's upper half, the sequence goes on");
    {
        ScriptedAccess a{.ackAfter = 0};
        a.words[2] = (7U << 16) | 41U;   // an earlier request that asked for 0.7 s
        CHECK(announceVia(a, true, Small, 9'350) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], (94U << 16) | 42U);   // 9.35 s rounded up to 9.4 s, sequence + 1
        CHECK(announceVia(a, true, Small) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], 43U);   // no time asked: the printer's default
        a.words[2] = 0xFFFFU;
        CHECK(announceVia(a, true, Small, 10'000'000) == Outcome::acknowledged);
        CHECK_EQ(a.words[2], (0xFFFFU << 16) | 1U);   // clamped to 6553.5 s, sequence never 0
    }

    Kvasir::Test::test("on the real (volatile) block");
    {
        Block volatile block{Magic, HostArmed, 0, 0};
        CHECK(announceOn(block, true, Small) == Outcome::timedOut);
        CHECK_EQ(block.request, 1U);
        block.ack = 2;   // a printer that already answers the next one
        CHECK(announceOn(block, true, Small) == Outcome::acknowledged);
        CHECK_EQ(block.request, 2U);
        CHECK(announceOn(block, false, Small) == Outcome::notArmed);
        CHECK_EQ(block.request, 2U);
    }

    Kvasir::Test::test("the firmware's block starts unarmed, request == ack");
    {
        CHECK_EQ(kvasir_announced_reset.magic, Magic);
        CHECK_EQ(kvasir_announced_reset.armed, 0U);
        CHECK_EQ(kvasir_announced_reset.request, kvasir_announced_reset.ack);
        CHECK(announce() == Outcome::notArmed);   // no printer armed it
    }

    Kvasir::Test::test(
      "waitForPrinterOn: false when nobody arms the block, true once a printer has");
    {
        Block volatile block{Magic, 0, 0, 0};
        CHECK(!printerListeningOn(block));
        CHECK(!waitForPrinterOn(block, 10));   // a short bound: nobody arms it here
        block.armed = HostArmed;
        CHECK(printerListeningOn(block));
        CHECK(waitForPrinterOn(block, 10));
    }

#if !defined(USE_UC_LOG)
    Kvasir::Test::test(
      "without USE_UC_LOG the public calls are constants, even with the block armed");
    {
        kvasir_announced_reset.armed = HostArmed;
        CHECK(announce() == Outcome::notArmed);
        CHECK(announce(4'000) == Outcome::notArmed);
        CHECK(!printerListening());
        CHECK(!waitForPrinter(10));
        CHECK_EQ(kvasir_announced_reset.request, 0U);   // untouched
        kvasir_announced_reset.armed = 0;
    }
#endif

    Kvasir::Test::test("the bounds: >= 0.5 s / 0.1 s at the fastest clock");
    {
        using namespace Kvasir::AnnouncedReset::detail;
        CHECK(static_cast<std::uint64_t>(DefaultPolls.ack) * MinCyclesPerPoll
              >= MaxCoreHz / 2 - MinCyclesPerPoll);
        CHECK(static_cast<std::uint64_t>(DefaultPolls.grace) * MinCyclesPerPoll
              >= MaxCoreHz / 10 - MinCyclesPerPoll);
    }

    return Kvasir::Test::report();
}
