// Runtime/CrashLog.hpp under the default records: each formatter writes its lines (a fault: three, a panic: one), and
// none takes a record. Built twice: without USE_UC_LOG (the formatters are empty), and with USE_UC_LOG against
// support/uc_log_stub, which records one letter per line.
#ifdef USE_UC_LOG
    #include "uc_log/uc_log.hpp"   // the stub (tests/support/uc_log_stub)
#endif
#include "kvasir/Runtime/CrashLog.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {
#ifdef USE_UC_LOG
static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

std::string_view lines() { return stub::events; }

void clearLines() { stub::events.clear(); }
#else
std::string_view lines() { return {}; }

void clearLines() {}
#endif

constexpr std::string_view logged(std::string_view withLog) {
#ifdef USE_UC_LOG
    return withLog;
#else
    return withLog.substr(0, 0);
#endif
}

constexpr Kvasir::Fault::FullRecord full{.count     = 2,
                                         .flags     = 0,
                                         .r0        = 10,
                                         .r1        = 11,
                                         .r2        = 12,
                                         .r3        = 13,
                                         .r12       = 14,
                                         .lr        = 15,
                                         .pc        = 16,
                                         .xpsr      = 17,
                                         .r4        = 0,
                                         .r5        = 0,
                                         .r6        = 0,
                                         .r7        = 0,
                                         .r8        = 0,
                                         .r9        = 0,
                                         .r10       = 0,
                                         .r11       = 0,
                                         .excReturn = 18,
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
}   // namespace

int main() {
    using Kvasir::Test::test;

    test("the formatters: a fault three lines, a panic one; no record taken");
    clearLines();
    Kvasir::Fault::lastFault.store(full);
    Kvasir::Fault::logFault(full);
    Kvasir::Panic::logPanic(Kvasir::Panic::FullRecord{.count    = 1,
                                                      .cause    = 0,
                                                      .pc       = 0x1000'0300U,
                                                      .detail   = 0,
                                                      .core     = 0,
                                                      .sp       = 0,
                                                      .timeLow  = 0,
                                                      .timeHigh = 0});
    CHECK(lines() == logged("EEEE"));
    CHECK(Kvasir::Fault::lastFault.valid());

    return Kvasir::Test::report();
}
