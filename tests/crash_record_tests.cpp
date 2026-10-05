// Kvasir::CrashRecord: a specialisation of injectedPolicy<> made after the headers (as an application
// does, next to its Startup list) switches Panic's record to the CRC-checked v2 record; the first
// panic is kept and later ones counted; takeLastPanic() keeps its shape and reads once.
#include "kvasir/Util/Panic.hpp"

template<>
inline constexpr auto Kvasir::CrashRecord::injectedPolicy<>
  = Kvasir::CrashRecord::Full<{.stackBytes = 64}>{};

#include "test_harness.hpp"

using Kvasir::Panic::Cause;

static_assert(
  Kvasir::CrashRecord::Detail::isFull<Kvasir::CrashRecord::Detail::PolicyFor<struct AnyAnchor>>);
static_assert(Kvasir::CrashRecord::Detail::PolicyFor<struct AnyAnchor>::options.stackBytes == 64);
static_assert(!Kvasir::CrashRecord::Detail::isFull<Kvasir::CrashRecord::Legacy>);
static_assert(Kvasir::Panic::Detail::fullPolicy<>);

int main() {
    using Kvasir::Test::test;

    test("nothing recorded");
    Kvasir::Panic::lastPanicV2.clear();
    CHECK(!Kvasir::Panic::Detail::panicRecorded());
    CHECK(!Kvasir::Panic::takeLastPanic());

    test("the first panic kept, later ones counted, in the v2 record");
    Kvasir::Panic::record(
      Kvasir::Panic::Info{.cause = Cause::stackSmash, .pc = 0x1000'3A5EU, .detail = 7});
    Kvasir::Panic::record(Kvasir::Panic::Info{.cause = Cause::abort, .pc = 0x2000'0000U});
    CHECK(Kvasir::Panic::Detail::panicRecorded());
    CHECK(Kvasir::Panic::lastPanic.magic
          != Kvasir::Panic::Record::Magic);   // the legacy one untouched
    auto const v2 = Kvasir::Panic::lastPanicV2.load();
    CHECK(v2 && v2->count == 2 && v2->cause == static_cast<std::uint32_t>(Cause::stackSmash)
          && v2->pc == 0x1000'3A5EU && v2->detail == 7 && v2->core == 0);

    test("takeLastPanic: the same shape, once");
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->count == 2 && r->cause == static_cast<std::uint32_t>(Cause::stackSmash)
          && r->pc == 0x1000'3A5EU && r->detail == 7);
    CHECK(!Kvasir::Panic::takeLastPanic());
    CHECK(!Kvasir::Panic::Detail::panicRecorded());

    return Kvasir::Test::report();
}
