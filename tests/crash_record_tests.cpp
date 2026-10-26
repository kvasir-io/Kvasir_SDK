// Kvasir::CrashRecord: a specialisation of the hook RecordFormat made after the headers (as an application
// does, next to its Startup list) wins over the default Full<>; the first panic is kept and later ones
// counted; takeLastPanic() keeps its shape and reads once. The chip's free-running counter (Port::FreeRunningUs,
// specialised after the headers as a chip package does) gives the first panic's time, both words.
#include "kvasir/Runtime/Panic.hpp"

#include <cstdint>

template<>
struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> {
    using type = Kvasir::CrashRecord::Full<{.stackBytes = 64}>;
};

namespace {
std::uint64_t fakeUs = 0;   // the counter the hook reads
}   // namespace

template<typename... Ts>
struct Kvasir::Port::FreeRunningUs<Kvasir::Tag::User, Ts...> {
    std::uint64_t operator()() const { return fakeUs; }
};

#include "test_harness.hpp"

#include <type_traits>

using Kvasir::Panic::Cause;

// the late specialisation wins wherever the lookup is dependent: Records<Anchor> is Full here
static_assert(std::is_same_v<Kvasir::CrashRecord::Records<struct AnyAnchor>,
                             Kvasir::CrashRecord::Full<{.stackBytes = 64}>>);
static_assert(Kvasir::CrashRecord::Records<struct AnyAnchor>::options.stackBytes == 64);
static_assert(std::is_same_v<Kvasir::Panic::Detail::PanicPartOf<>,
                             Kvasir::CrashRecord::Full<{.stackBytes = 64}>::Panic>);
static_assert(Kvasir::CrashRecord::PanicPart<Kvasir::CrashRecord::Full<>::Panic>);
static_assert(!Kvasir::CrashRecord::Format<int>);

int main() {
    // the image's format, after its specialisation: what BootGuard and the boot report read
    using Records = Kvasir::CrashRecord::Records<struct RecordsAnchor>;
    using Kvasir::Test::test;

    test("nothing recorded");
    Kvasir::Panic::lastPanic.clear();
    CHECK(!Records::Panic::recorded());
    CHECK(!Kvasir::Panic::takeLastPanic());

    test("the first panic kept with its time, later ones counted");
    static_assert(Kvasir::Panic::Detail::hasFreeRunningUs<struct HookAnchor>);
    fakeUs = 0x0000'0003'8000'0123ULL;   // past 2^32 us: the high word counts
    Kvasir::Panic::record(
      Kvasir::Panic::Info{.cause = Cause::stackSmash, .pc = 0x1000'3A5EU, .detail = 7});
    fakeUs = 0x0000'0004'0000'0000ULL;
    Kvasir::Panic::record(Kvasir::Panic::Info{.cause = Cause::abort, .pc = 0x2000'0000U});
    CHECK(Records::Panic::recorded());
    auto const full = Kvasir::Panic::lastPanic.load();
    CHECK(full && full->count == 2 && full->cause == static_cast<std::uint32_t>(Cause::stackSmash)
          && full->pc == 0x1000'3A5EU && full->detail == 7 && full->core == 0);
    CHECK(full && full->timeLow == 0x8000'0123U && full->timeHigh == 3U
          && full->timeUs() == 0x0000'0003'8000'0123ULL);

    test("takeLastPanic: the record, once");
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->count == 2 && r->cause == static_cast<std::uint32_t>(Cause::stackSmash)
          && r->pc == 0x1000'3A5EU && r->detail == 7 && r->timeUs() == 0x0000'0003'8000'0123ULL);
    CHECK(!Kvasir::Panic::takeLastPanic());
    CHECK(!Records::Panic::recorded());

    return Kvasir::Test::report();
}
