// Kvasir::ImageCheck (Runtime/ImageCheck.hpp) on a fake flash: the Software backend over a descriptor built the way
// patch_image_crc.py builds one. One pass gives the descriptor's CRC and every chunk size agrees (unaligned segment
// starts and lengths too); runPass(); a flipped byte (KVASIR_IMAGE_CRC_TEST) is a corrupt pass that calls the policy
// once with (got, want) and the next pass is ok again; an unpatched descriptor stops the checker without calling the
// policy; Paced works once per period; consecutiveCorrupt() counts corrupt passes in a row; OnMismatch::Call gives a
// three-argument function that count; OnMismatch::Panic raises imageCorrupt with the computed CRC,
// OnMismatch::PanicAfter<N> only at the N-th in a row and never while debugged.
// Their log lines and the fatal start (beginFatal) are in image_check_fatal_tests.cpp.
#include "harness/PanicCatcher.hpp"
#include "kvasir/Runtime/ImageCheck.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace IC = Kvasir::ImageCheck;
using namespace std::chrono_literals;

namespace {
using Kvasir::Test::panicBack;
using Kvasir::Test::panicSeen;

// the fake flash: addresses are offsets into it
std::array<std::uint8_t, 4096> flash{};

struct FakeRead {
    static std::uint32_t word(std::uintptr_t a) {
        return static_cast<std::uint32_t>(flash[a] | (flash[a + 1] << 8U) | (flash[a + 2] << 16U)
                                          | (static_cast<std::uint32_t>(flash[a + 3]) << 24U));
    }

    static std::uint8_t byte(std::uintptr_t a) { return flash[a]; }
};

// zlib's crc32, bit by bit: independent of Crc.hpp's tables
std::uint32_t zlibCrc(std::uint32_t       crc,
                      std::uint8_t const* p,
                      std::size_t         n) {
    crc = ~crc;
    for(std::size_t i = 0; i != n; ++i) {
        crc ^= p[i];
        for(int k = 0; k != 8; ++k) {
            crc = (crc & 1U) != 0 ? 0xEDB8'8320U ^ (crc >> 1U) : crc >> 1U;
        }
    }
    return ~crc;
}

// three segments with gaps between them, odd starts and lengths; the gaps are never read
constexpr std::array<IC::Segment, 3> segments{
  {{0x010, 1000}, {0x403, 517}, {0x800, 2045}}
};

std::uint32_t expectedCrc() {
    std::uint32_t crc = 0;
    for(auto const& s : segments) { crc = zlibCrc(crc, flash.data() + s.address, s.length); }
    return crc;
}

struct FakeSource {
    static constexpr bool configured = true;
    static inline IC::Descriptor volatile d{};

    static IC::Descriptor const volatile& get() { return d; }

    static void patch(std::uint32_t magic) {
        d.magic = magic;
        d.crc   = expectedCrc();
        d.count = segments.size();
        for(std::size_t i = 0; i != segments.size(); ++i) {
            d.segments[i].address = segments[i].address;
            d.segments[i].length  = segments[i].length;
        }
    }
};

struct Seen {
    static inline int           calls = 0;
    static inline std::uint32_t got   = 0;
    static inline std::uint32_t want  = 0;

    static void mismatch(std::uint32_t g,
                         std::uint32_t w) {
        ++calls;
        got  = g;
        want = w;
    }
};

using Clock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;

// a OnMismatch::Call function that asks for the count of corrupt passes in a row as well
struct SeenInARow {
    static inline std::uint32_t got   = 0;
    static inline std::uint32_t want  = 0;
    static inline std::uint32_t inRow = 0;

    static void mismatch(std::uint32_t g,
                         std::uint32_t w,
                         std::uint32_t n) {
        got   = g;
        want  = w;
        inRow = n;
    }
};

struct FakeDebug {
    static inline bool on = false;

    static bool attached() { return on; }
};

using Sw = IC::Software<16, FakeRead>;
template<std::size_t Chunk>
using EveryTurnChecker = IC::Checker<Sw, IC::EveryTurn<Chunk>, FakeSource>;

// a hardware backend's Startup resources come with its Checker (the Startup list holds the Checker); the software one
// has none
struct ClaimingBackend {
    struct Claim;
    struct Provide;
    using Claims   = Claim;
    using Provides = Provide;

    static void begin() {}

    static bool chunk(std::uintptr_t,
                      std::size_t) {
        return true;
    }

    static std::uint32_t result() { return 0; }
};

static_assert(std::is_same_v<IC::Checker<ClaimingBackend,
                                         IC::EveryTurn<>,
                                         FakeSource>::Claims,
                             ClaimingBackend::Claim>);
static_assert(std::is_same_v<IC::Checker<ClaimingBackend,
                                         IC::EveryTurn<>,
                                         FakeSource>::Provides,
                             ClaimingBackend::Provide>);
template<typename C>
concept HasResources = requires {
    typename C::Claims;
    typename C::Provides;
};

static_assert(!HasResources<EveryTurnChecker<16>>);

// turns until the pass count moves, at most `limit`
template<typename C>
int turnsToPass(int limit = 100'000) {
    auto const before = C::passes();
    int        turns  = 0;
    while(C::passes() == before && turns < limit) {
        C::turn();
        ++turns;
    }
    return turns;
}
}   // namespace

template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::CatchPanic;
};

template<>
struct Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User> {
    using type = IC::OnMismatch::Call<&Seen::mismatch>;
};

int main() {
    using Kvasir::Test::test;
    for(std::size_t i = 0; i != flash.size(); ++i) {
        flash[i] = static_cast<std::uint8_t>(i * 7U + (i >> 5U));
    }
    FakeSource::patch(IC::Descriptor::magicValue);
    std::uint32_t const want  = expectedCrc();
    std::size_t         bytes = 0;
    for(auto const& s : segments) { bytes += s.length; }

    test("one pass gives the descriptor's CRC; every chunk size agrees");
    CHECK(EveryTurnChecker<1024>::lastResult() == IC::Result::unknown);
    CHECK(turnsToPass<EveryTurnChecker<1024>>() == static_cast<int>((bytes + 1023) / 1024));
    CHECK(EveryTurnChecker<1024>::lastResult() == IC::Result::ok);
    CHECK(EveryTurnChecker<1024>::lastCrc() == want && EveryTurnChecker<1024>::passes() == 1);
    CHECK(turnsToPass<EveryTurnChecker<1>>() == static_cast<int>(bytes));
    CHECK(EveryTurnChecker<1>::lastCrc() == want);
    CHECK(turnsToPass<EveryTurnChecker<3>>() == static_cast<int>((bytes + 2) / 3));
    CHECK(EveryTurnChecker<3>::lastCrc() == want);
    CHECK(turnsToPass<EveryTurnChecker<1'000'000>>() == 1);
    CHECK(EveryTurnChecker<1'000'000>::lastCrc() == want);
    CHECK(Seen::calls == 0);

    test("passes repeat and count; runPass does a whole one");
    CHECK(turnsToPass<EveryTurnChecker<1024>>() > 1);
    CHECK(EveryTurnChecker<1024>::passes() == 2
          && EveryTurnChecker<1024>::lastResult() == IC::Result::ok);
    CHECK(EveryTurnChecker<1024>::runPass() == IC::Result::ok
          && EveryTurnChecker<1024>::passes() == 3);

    test("a flipped byte: corrupt, the policy once with (got, want); the next pass is ok again");
    IC::Test::corruptNextPass(segments[1].address + 100);
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::corrupt);
    CHECK(Seen::calls == 1 && Seen::want == want && Seen::got != want);
    CHECK(Seen::got == EveryTurnChecker<256>::lastCrc());
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::ok && Seen::calls == 1);
    CHECK(EveryTurnChecker<256>::consecutiveCorrupt() == 0);
    // a byte in a gap is never read: nothing to flip
    IC::Test::corruptNextPass(segments[0].address + segments[0].length + 1);
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::ok && Seen::calls == 1);
    IC::Test::corruptNextPass(0);

    test("consecutiveCorrupt counts corrupt passes in a row; an ok pass ends the run");
    using InARow = IC::Checker<Sw, IC::EveryTurn<256>, FakeSource>;
    IC::Test::corruptNextPass(segments[2].address + 7);
    CHECK(InARow::runPass() == IC::Result::corrupt && InARow::consecutiveCorrupt() == 1);
    IC::Test::corruptNextPass(segments[0].address);
    CHECK(InARow::runPass() == IC::Result::corrupt && InARow::consecutiveCorrupt() == 2);
    CHECK(InARow::runPass() == IC::Result::ok && InARow::consecutiveCorrupt() == 0);
    CHECK(Seen::calls == 3);
    IC::Test::corruptNextPass(0);

    test("corruptPasses(address, 2): the byte read flipped in two passes in a row, then as it is");
    IC::Test::corruptPasses(segments[1].address + 3, 2);
    CHECK(InARow::runPass() == IC::Result::corrupt && InARow::consecutiveCorrupt() == 1);
    auto const firstBad = InARow::lastCrc();
    CHECK(InARow::runPass() == IC::Result::corrupt && InARow::consecutiveCorrupt() == 2);
    CHECK(InARow::lastCrc() == firstBad);
    CHECK(InARow::runPass() == IC::Result::ok && InARow::consecutiveCorrupt() == 0);
    CHECK(Seen::calls == 5);

    test(
      "OnMismatch::Call hands a three-argument function the count in a row, a two-argument one "
      "(Seen) "
      "not");
    IC::OnMismatch::Call<&SeenInARow::mismatch>::mismatch(0xAAAA'0001U, 0xBBBB'0002U, 7);
    CHECK(SeenInARow::got == 0xAAAA'0001U && SeenInARow::want == 0xBBBB'0002U
          && SeenInARow::inRow == 7);
    IC::OnMismatch::Call<&Seen::mismatch>::mismatch(0xAAAA'0003U, want, 9);
    CHECK(Seen::calls == 6 && Seen::got == 0xAAAA'0003U && Seen::want == want);

    test("an unpatched descriptor: noDescriptor, stopped, no policy call");
    FakeSource::d.magic = IC::Descriptor::unpatched;
    using Fresh         = IC::Checker<Sw, IC::EveryTurn<64>, FakeSource>;
    for(int i = 0; i < 1000; ++i) { Fresh::turn(); }
    CHECK(Fresh::lastResult() == IC::Result::noDescriptor && Fresh::passes() == 0
          && Seen::calls == 6);
    FakeSource::d.count
      = IC::Descriptor::maxSegments + 1;   // a count past the table is no descriptor either
    FakeSource::d.magic = IC::Descriptor::magicValue;
    using TooMany       = IC::Checker<Sw, IC::EveryTurn<65>, FakeSource>;
    TooMany::turn();
    CHECK(TooMany::lastResult() == IC::Result::noDescriptor);
    FakeSource::patch(IC::Descriptor::magicValue);

    test("Paced: one chunk per period");
    using Paced = IC::Checker<Sw, IC::Paced<Clock, 10, 512>, FakeSource>;
    Clock::reset();
    Paced::turn();   // the first call is due
    for(int i = 0; i < 9; ++i) {
        Clock::advance(1ms);
        Paced::turn();
    }
    CHECK(Paced::passes() == 0);
    int periods = 1;
    while(Paced::passes() == 0 && periods < 100) {
        Clock::advance(10ms);
        Paced::turn();
        ++periods;
    }
    CHECK(periods == static_cast<int>((bytes + 511) / 512) && Paced::lastCrc() == want);

#ifndef KVASIR_TEST_NO_LONGJMP
    test("OnMismatch::Panic raises imageCorrupt with the computed CRC");
    panicSeen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(panicBack) == 0) { IC::OnMismatch::Panic::mismatch(0x1234'5678U, want); }
    CHECK(panicSeen && panicSeen->cause == Kvasir::Panic::Cause::imageCorrupt
          && panicSeen->detail == 0x1234'5678U);

    test(
      "OnMismatch::PanicAfter<3>: not before the third in a row, then imageCorrupt; never while "
      "debugged");
    using After = IC::OnMismatch::PanicAfter<3, FakeDebug>;
    static_assert(After::limit == 3);
    panicSeen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(panicBack) == 0) {
        After::mismatch(0xBAD0'0001U, want, 1);
        After::mismatch(0xBAD0'0002U, want, 2);
    }
    CHECK(!panicSeen);
    FakeDebug::on = true;
    if(setjmp(panicBack) == 0) { After::mismatch(0xBAD0'0003U, want, 3); }
    CHECK(!panicSeen);
    FakeDebug::on = false;
    if(setjmp(panicBack) == 0) { After::mismatch(0xBAD0'0004U, want, 4); }
    CHECK(panicSeen && panicSeen->cause == Kvasir::Panic::Cause::imageCorrupt
          && panicSeen->detail == 0xBAD0'0004U);
#endif
    CHECK(std::string_view{Kvasir::Panic::name(Kvasir::Panic::Cause::imageCorrupt)}
          == "image CRC mismatch");

    return Kvasir::Test::report();
}
