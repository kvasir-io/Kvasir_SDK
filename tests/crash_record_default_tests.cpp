// Kvasir::CrashRecord's default format (no specialisation): Full<> - a panic goes into the CRC-checked
// Panic::lastPanic, the first kept and later ones counted, taken once; a fault record in Fault::lastFault is taken
// once by takeLastFault().
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include "test_harness.hpp"

#include <type_traits>

using Kvasir::Panic::Cause;

static_assert(std::is_same_v<Kvasir::CrashRecord::Records<struct AnyAnchor>,
                             Kvasir::CrashRecord::Full<>>);
static_assert(Kvasir::CrashRecord::Records<struct AnyAnchor>::options.stackBytes == 0);
static_assert(Kvasir::CrashRecord::Records<struct AnyAnchor>::options.clearOnPowerOn);
static_assert(Kvasir::CrashRecord::FaultPart<Kvasir::CrashRecord::Full<>::Fault>);
static_assert(Kvasir::CrashRecord::PanicPart<Kvasir::CrashRecord::Full<>::Panic>);

int main() {
    // the image's format, after its specialisation: what BootGuard and the boot report read
    using Records = Kvasir::CrashRecord::Records<struct RecordsAnchor>;
    using Kvasir::Test::test;

    test("default: nothing recorded");
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Fault::lastFault.clear();
    CHECK(!Records::Panic::recorded());
    CHECK(!Records::Panic::cause());
    CHECK(!Records::Fault::recorded());

    test("default: the first panic kept, the second counted, in the CRC-checked record");
    Kvasir::Panic::record(
      Kvasir::Panic::Info{.cause = Cause::stackSmash, .pc = 0x1000'0100U, .detail = 5});
    Kvasir::Panic::record(Kvasir::Panic::Info{.cause = Cause::abort, .pc = 0x1000'0200U});
    CHECK(Records::Panic::recorded());
    CHECK(Records::Panic::cause() == static_cast<std::uint32_t>(Cause::stackSmash));
    auto const full = Kvasir::Panic::lastPanic.load();
    CHECK(full && full->count == 2 && full->pc == 0x1000'0100U && full->detail == 5
          && full->core == 0);
    // no Port::FreeRunningUs here: no time
    static_assert(!Kvasir::Panic::Detail::hasFreeRunningUs<struct NoHookAnchor>);
    CHECK(full && full->timeLow == 0 && full->timeHigh == 0 && full->timeUs() == 0);

    test("default: takeLastPanic once");
    auto const p = Kvasir::Panic::takeLastPanic();
    CHECK(p && p->count == 2 && p->pc == 0x1000'0100U && p->detail == 5 && p->timeUs() == 0);
    CHECK(!Kvasir::Panic::takeLastPanic());

    test("default: takeLastFault once");
    Kvasir::Fault::FullRecord f{};
    f.count = 1;
    f.pc    = 0x1000'0300U;
    Kvasir::Fault::lastFault.store(f);
    CHECK(Records::Fault::recorded());
    auto const r = Kvasir::Fault::takeLastFault();
    CHECK(r && r->pc == 0x1000'0300U && r->count == 1U);
    CHECK(!Kvasir::Fault::takeLastFault());
    CHECK(!Records::Fault::recorded());

    return Kvasir::Test::report();
}
