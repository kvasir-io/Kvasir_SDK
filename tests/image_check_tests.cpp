// Kvasir::ImageCheck (Util/ImageCheck.hpp) on a fake flash: the Software backend over a descriptor built the way
// patch_image_crc.py builds one. One pass gives the descriptor's CRC and every chunk size agrees (unaligned segment
// starts and lengths too); runPass(); a flipped byte (KVASIR_IMAGE_CRC_TEST) is a corrupt pass that calls the policy
// once with (got, want) and the next pass is ok again; an unpatched descriptor stops the checker without calling the
// policy; Paced works once per period; Policy::Panic raises flashCorrupt with the computed CRC.
#include "kvasir/Util/ImageCheck.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"

#include <array>
#include <chrono>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Kvasir::Panic {
[[noreturn,
  gnu::noinline]] void
raise(Cause cause) {
    Detail::dispatch(Info{
      cause,
      static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)))});
}

[[noreturn]] void raiseAt(Cause         cause,
                          std::uint32_t pc,
                          std::uint32_t detail) {
    Detail::dispatch(Info{cause, pc, detail});
}
}   // namespace Kvasir::Panic

namespace IC = Kvasir::ImageCheck;
using namespace std::chrono_literals;

namespace {
std::jmp_buf                       back;
std::optional<Kvasir::Panic::Info> seen;

struct TestHandler {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        seen = info;
        std::longjmp(back, 1);
    }
};

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
constexpr std::array<IC::Segment, 3> Segments{
  {{0x010, 1000}, {0x403, 517}, {0x800, 2045}}
};

std::uint32_t expectedCrc() {
    std::uint32_t crc = 0;
    for(auto const& s : Segments) { crc = zlibCrc(crc, flash.data() + s.address, s.length); }
    return crc;
}

struct FakeSource {
    static constexpr bool configured = true;
    static inline IC::Descriptor volatile d{};

    static IC::Descriptor const volatile& get() { return d; }

    static void patch(std::uint32_t magic) {
        d.magic   = magic;
        d.version = IC::Descriptor::Version;
        d.crc     = expectedCrc();
        d.count   = Segments.size();
        for(std::size_t i = 0; i != Segments.size(); ++i) {
            d.segments[i].address = Segments[i].address;
            d.segments[i].length  = Segments[i].length;
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

using Sw = IC::Software<16, FakeRead>;
template<std::size_t Chunk>
using EveryTurnChecker = IC::Checker<Sw, IC::EveryTurn<Chunk>, FakeSource>;

// turns until the pass count moves, at most `limit`
template<typename C>
int turnsToPass(int limit = 100'000) {
    auto const before = C::passes();
    int        turns  = 0;
    while(C::passes() == before && turns < limit) {
        C::step();
        ++turns;
    }
    return turns;
}
}   // namespace

template<>
inline constexpr auto Kvasir::Panic::injectedHandler<> = TestHandler{};
template<>
inline constexpr auto IC::injectedMismatchPolicy<> = IC::Policy::Call<&Seen::mismatch>{};

// TSan's longjmp interceptor aborts on a jump out of a function that does not return; this test has no threads for
// TSan to judge.
#if defined(__SANITIZE_THREAD__)
    #define IMAGE_CHECK_TEST_NO_LONGJMP 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define IMAGE_CHECK_TEST_NO_LONGJMP 1
    #endif
#endif

int main() {
    using Kvasir::Test::test;
    for(std::size_t i = 0; i != flash.size(); ++i) {
        flash[i] = static_cast<std::uint8_t>(i * 7U + (i >> 5U));
    }
    FakeSource::patch(IC::Descriptor::Magic);
    std::uint32_t const want  = expectedCrc();
    std::size_t         bytes = 0;
    for(auto const& s : Segments) { bytes += s.length; }

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
    IC::Test::corruptNextPass(Segments[1].address + 100);
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::corrupt);
    CHECK(Seen::calls == 1 && Seen::want == want && Seen::got != want);
    CHECK(Seen::got == EveryTurnChecker<256>::lastCrc());
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::ok && Seen::calls == 1);
    // a byte in a gap is never read: nothing to flip
    IC::Test::corruptNextPass(Segments[0].address + Segments[0].length + 1);
    CHECK(EveryTurnChecker<256>::runPass() == IC::Result::ok && Seen::calls == 1);
    IC::Test::corruptNextPass(0);

    test("an unpatched descriptor: noDescriptor, stopped, no policy call");
    FakeSource::d.magic = IC::Descriptor::Unpatched;
    using Fresh         = IC::Checker<Sw, IC::EveryTurn<64>, FakeSource>;
    for(int i = 0; i < 1000; ++i) { Fresh::step(); }
    CHECK(Fresh::lastResult() == IC::Result::noDescriptor && Fresh::passes() == 0
          && Seen::calls == 1);
    FakeSource::d.count
      = IC::Descriptor::MaxSegments + 1;   // a count past the table is no descriptor either
    FakeSource::d.magic = IC::Descriptor::Magic;
    using TooMany       = IC::Checker<Sw, IC::EveryTurn<65>, FakeSource>;
    TooMany::step();
    CHECK(TooMany::lastResult() == IC::Result::noDescriptor);
    FakeSource::patch(IC::Descriptor::Magic);

    test("Paced: one chunk per period");
    using Paced = IC::Checker<Sw, IC::Paced<Clock, 10, 512>, FakeSource>;
    Clock::reset();
    Paced::step();   // the first call is due
    for(int i = 0; i < 9; ++i) {
        Clock::advance(1ms);
        Paced::step();
    }
    CHECK(Paced::passes() == 0);
    int periods = 1;
    while(Paced::passes() == 0 && periods < 100) {
        Clock::advance(10ms);
        Paced::step();
        ++periods;
    }
    CHECK(periods == static_cast<int>((bytes + 511) / 512) && Paced::lastCrc() == want);

#ifndef IMAGE_CHECK_TEST_NO_LONGJMP
    test("Policy::Panic raises flashCorrupt with the computed CRC");
    seen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(back) == 0) { IC::Policy::Panic::mismatch(0x1234'5678U, want); }
    CHECK(seen && seen->cause == Kvasir::Panic::Cause::flashCorrupt
          && seen->detail == 0x1234'5678U);
#endif
    CHECK(std::string_view{Kvasir::Panic::name(Kvasir::Panic::Cause::flashCorrupt)}
          == "flash image CRC mismatch");

    return Kvasir::Test::report();
}
