// Runtime/BootLog.hpp with the stub uc_log (one letter per line, tests/support/uc_log_stub) and the real generated
// cmake_git_version headers of this tree: takePrevious() takes both records once and drops them after a power-on,
// endedInFailure() for none / a fault / a panic, the boot line's level (DEBUG, WARN after a failure), the records'
// lines after it, and the chip's two late hooks (Port::RestartedAfterJump: one WARN line; Port::StartNotes: its own
// lines) in that order.
#include "kvasir/Runtime/BootLog.hpp"
#include "test_harness.hpp"
#include "uc_log/uc_log.hpp"   // the stub (tests/support/uc_log_stub)

#include <cstdint>
#include <string_view>

static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

namespace {
enum class Cause : std::uint8_t { powerOn, watchdog };

bool jumped = false;
}   // namespace

// The chip's hooks, specialised after the header (as a chip package's StartUp.hpp does) and looked up late.
template<>
struct Kvasir::Port::RestartedAfterJump<Kvasir::Tag::User> {
    bool operator()() const { return jumped; }
};

template<>
struct Kvasir::Port::StartNotes<Kvasir::Tag::User> {
    void operator()() const { stub::record('N', "note"); }
};

namespace {
using Kvasir::Boot::Previous;

constexpr Kvasir::Fault::FullRecord fault{.count     = 1,
                                          .flags     = 0,
                                          .r0        = 0,
                                          .r1        = 0,
                                          .r2        = 0,
                                          .r3        = 0,
                                          .r12       = 0,
                                          .lr        = 0,
                                          .pc        = 0x1000'0200U,
                                          .xpsr      = 0,
                                          .r4        = 0,
                                          .r5        = 0,
                                          .r6        = 0,
                                          .r7        = 0,
                                          .r8        = 0,
                                          .r9        = 0,
                                          .r10       = 0,
                                          .r11       = 0,
                                          .excReturn = 0,
                                          .msp       = 0,
                                          .psp       = 0,
                                          .control   = 0,
                                          .sp        = 0,
                                          .cfsr      = 0,
                                          .hfsr      = 0,
                                          .mmfar     = 0,
                                          .bfar      = 0,
                                          .shcsr     = 0,
                                          .icsr      = 0,
                                          .sfsr      = 0,
                                          .sfar      = 0,
                                          .msplim    = 0,
                                          .psplim    = 0};
constexpr Kvasir::Panic::FullRecord panic{.count    = 1,
                                          .cause    = 0,
                                          .pc       = 0x1000'0300U,
                                          .detail   = 0,
                                          .core     = 0,
                                          .sp       = 0,
                                          .timeLow  = 0,
                                          .timeHigh = 0};

std::string_view lines() { return stub::events; }

void clearAll() {
    stub::events.clear();
    Kvasir::Fault::lastFault.clear();
    Kvasir::Panic::lastPanic.clear();
    jumped = false;
}

void takePrevious() {
    using Kvasir::Test::test;
    test("takePrevious: both records, once");
    clearAll();
    Kvasir::Fault::lastFault.store(fault);
    Kvasir::Panic::lastPanic.store(panic);
    auto const p = Kvasir::Boot::takePrevious();
    CHECK(p.fault && p.fault->pc == 0x1000'0200U);
    CHECK(p.panic && p.panic->pc == 0x1000'0300U);
    auto const again = Kvasir::Boot::takePrevious();
    CHECK(!again.fault && !again.panic);

    test("takePrevious(powerOn): dropped unread (clearOnPowerOn), and taken all the same");
    Kvasir::Fault::lastFault.store(fault);
    Kvasir::Panic::lastPanic.store(panic);
    auto const dropped = Kvasir::Boot::takePrevious(true);
    CHECK(!dropped.fault && !dropped.panic);
    CHECK(!Kvasir::Fault::lastFault.valid() && !Kvasir::Panic::lastPanic.valid());
}

void endedInFailure() {
    using Kvasir::Test::test;
    test("endedInFailure: none, a fault, a panic, both");
    CHECK(!Kvasir::Boot::endedInFailure(Previous{}));
    CHECK(Kvasir::Boot::endedInFailure(Previous{.fault = fault, .panic = {}}));
    CHECK(Kvasir::Boot::endedInFailure(Previous{.fault = {}, .panic = panic}));
    CHECK(Kvasir::Boot::endedInFailure(Previous{.fault = fault, .panic = panic}));
}

void logBoot() {
    using Kvasir::Test::test;
    test("logBoot: a routine boot is one DEBUG line, then the start notes");
    clearAll();
    Kvasir::Boot::logBoot(Cause::powerOn, Previous{});
    CHECK(lines() == "DN");

    test("logBoot: after a fault WARN, then the fault's three lines");
    clearAll();
    Kvasir::Boot::logBoot(Cause::watchdog, Previous{.fault = fault, .panic = {}});
    CHECK(lines() == "WNEEE");

    test("logBoot: after a panic WARN, then its line; a fault comes before the panic");
    clearAll();
    Kvasir::Boot::logBoot(Cause::watchdog, Previous{.fault = {}, .panic = panic});
    CHECK(lines() == "WNE");
    clearAll();
    Kvasir::Boot::logBoot(Cause::watchdog, Previous{.fault = fault, .panic = panic});
    CHECK(lines() == "WNEEEE");

    test("logBoot: a start after a jump adds the restart WARN before the notes");
    clearAll();
    jumped = true;
    Kvasir::Boot::logBoot(Cause::powerOn, Previous{});
    CHECK(lines() == "DWN");

    test("logBoot(cause): takes the records itself");
    clearAll();
    Kvasir::Fault::lastFault.store(fault);
    Kvasir::Boot::logBoot(Cause::watchdog);
    CHECK(lines() == "WNEEE");
    CHECK(!Kvasir::Fault::lastFault.valid());

    test("logPrevious: the records alone");
    clearAll();
    Kvasir::Boot::logPrevious(Previous{.fault = fault, .panic = panic});
    CHECK(lines() == "EEEE");
}
}   // namespace

int main() {
    takePrevious();
    endedInFailure();
    logBoot();
    return Kvasir::Test::report();
}
