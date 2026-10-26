// The record headers with CrashRecord::Full<> specialised after them (as an application does next to its Startup
// list): takeLastFault() takes lastFault, takeLastPanic() takes lastPanic - both looked up where they are
// called (the empty template pack), so the late specialisation is the one they use.
#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"

template<>
struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> {
    using type = Kvasir::CrashRecord::Full<>;
};

#include "kvasir/Runtime/CrashLog.hpp"
#include "test_harness.hpp"

int main() {
    // the image's format, after its specialisation: what BootGuard and the boot report read
    using Records = Kvasir::CrashRecord::Records<struct RecordsAnchor>;
    using Kvasir::Test::test;

    test("Full: takeLastFault takes lastFault, once");
    Kvasir::Fault::FullRecord f{};
    f.count = 1;
    f.pc    = 0x1000'0400U;
    Kvasir::Fault::lastFault.store(f);
    CHECK(Records::Fault::recorded());
    auto const r = Kvasir::Fault::takeLastFault();
    CHECK(r && r->pc == 0x1000'0400U && r->count == 1U);
    CHECK(!Records::Fault::recorded());
    CHECK(!Kvasir::Fault::lastFault.valid());

    test("Full: takeLastPanic takes lastPanic");
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Panic::record(
      Kvasir::Panic::Info{.cause = Kvasir::Panic::Cause::abort, .pc = 0x1000'0500U});
    CHECK(Records::Panic::recorded());
    auto const p = Kvasir::Panic::takeLastPanic();
    CHECK(p && p->pc == 0x1000'0500U && p->count == 1U);
    CHECK(!Records::Panic::recorded());

    return Kvasir::Test::report();
}
