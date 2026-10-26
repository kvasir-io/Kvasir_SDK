// The fatal-site primitive (Runtime/Panic.hpp): KVASIR_DETAIL_FATAL_AT, KVASIR_DETAIL_FATAL, KVASIR_PANIC keep the order
// start (the injected fatal start) - line - end, and KVASIR_DETAIL_CALLER_PC() names the caller of the function it
// expands in, on a 64-bit host too. Built with USE_UC_LOG against support/uc_log_stub, which records the level of each
// line; the fatal start records 'S' (beginFatal, and again in the dispatch before the handler), the handler 'R'
// (then leaves by longjmp, harness/PanicCatcher).
#include "kvasir/Runtime/Panic.hpp"

#include "harness/PanicCatcher.hpp"
#include "test_harness.hpp"
#include "uc_log/uc_log.hpp"   // the stub (tests/support/uc_log_stub)

#include <cstdint>
#include <string>
#include <string_view>

static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

// uc_log's catalog site wraps __COUNTER__ in _Pragma diagnostic push/pop (remote_fmt catalog.hpp). The primitive must
// expand a log line where it is used, not while pre-expanding a macro argument: there the _Pragma lines are dropped
// and __COUNTER__ warns. This stand-in has the same shape; the target compiles with -Werror -Wc2y-extensions (clang).
#ifdef __clang__
    #undef UC_LOG_C
    #define UC_LOG_C(...)                                                                     \
        _Pragma("clang diagnostic push")                                                      \
          _Pragma("clang diagnostic ignored \"-Wc2y-extensions\"")::stub::record('C',         \
                                                                                 __COUNTER__, \
                                                                                 __VA_ARGS__) \
            _Pragma("clang diagnostic pop")
#endif

namespace {
struct RecordStart {
    static void operator()() { stub::events += 'S'; }
};

struct RecordRaise {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        stub::events += 'R';
        Kvasir::Test::CatchPanic{}(info);
    }
};

using Kvasir::Panic::Cause;
}   // namespace

template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = RecordStart;
};

template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = RecordRaise;
};

namespace {
// a library end that panics for its caller: the site must be inside the function that called it
[[noreturn,
  gnu::noinline]] void
libraryEnd(int status) {
    KVASIR_DETAIL_FATAL_AT(Cause::abort,
                           KVASIR_DETAIL_CALLER_PC(),
                           static_cast<std::uint32_t>(status),
                           "end({}) called",
                           status);
}

[[gnu::noinline,
  maybe_unused]] void
callsLibraryEnd() {
    libraryEnd(3);
}   // the longjmp cases only

struct Pcs {
    std::uint32_t macro;
    std::uint32_t own;
};

[[gnu::noinline]] Pcs callerPcs() {
    return Pcs{KVASIR_DETAIL_CALLER_PC(),
               Kvasir::Panic::Detail::sitePc(__builtin_return_address(0))};
}

#ifndef KVASIR_TEST_NO_LONGJMP
void fatalAtWithALine() {
    stub::events.clear();
    char const        text[] = "abcdef";
    std::size_t const n      = 3;
    auto const        seen   = Kvasir::Test::panicOf([&] {
        KVASIR_DETAIL_FATAL_AT(Cause::abort,
                               0x1234U,
                               7U,
                               "x {} {}",
                               5,
                               std::string_view{text, n});   // a comma inside the line's arguments
    });
    CHECK(stub::events == "SCSR");
    CHECK(seen.has_value());
    if(seen) {
        CHECK(seen->cause == Cause::abort);
        CHECK(seen->pc == 0x1234U);
        CHECK(seen->detail == 7U);
    }
}

void fatalAtWithoutALine() {
    stub::events.clear();
    auto const seen
      = Kvasir::Test::panicOf([] { KVASIR_DETAIL_FATAL_AT(Cause::timeout, 0x10U, 0U); });
    CHECK(stub::events == "SSR");
    CHECK(seen.has_value() && seen->cause == Cause::timeout && seen->pc == 0x10U
          && seen->detail == 0U);
}

void fatalRunsStartStatementEnd() {
    stub::events.clear();
    auto const seen = Kvasir::Test::panicOf([] {
        KVASIR_DETAIL_FATAL(stub::events += 'L',
                            Kvasir::Panic::raiseAt(Cause::checkFailed, 0x20U, 1U));
    });
    CHECK(stub::events == "SLSR");
    CHECK(seen.has_value() && seen->cause == Cause::checkFailed && seen->pc == 0x20U
          && seen->detail == 1U);
}

void panicMacro() {
    stub::events.clear();
    auto const seen = Kvasir::Test::panicOf([] { KVASIR_PANIC("x {}", 1); });
    CHECK(stub::events == "SCSR");
    CHECK(seen.has_value() && seen->cause == Cause::user && seen->pc != 0U);
}

void siteIsTheCaller() {
    stub::events.clear();
    auto const seen = Kvasir::Test::panicOf([] { callsLibraryEnd(); });
    CHECK(stub::events == "SCSR");
    CHECK(seen.has_value());
    if(seen) {
        // the return address lies in callsLibraryEnd, past its start (not in libraryEnd)
        auto const fn
          = Kvasir::Panic::Detail::sitePc(reinterpret_cast<void const*>(&callsLibraryEnd));
        CHECK(seen->pc > fn);
        CHECK(seen->pc - fn < 0x1000U);
        CHECK(seen->detail == 3U);
    }
}
#endif

void callerPcCompilesOnTheHost() {
    auto const pcs = callerPcs();
    CHECK(pcs.macro == pcs.own);
    CHECK(pcs.macro != 0U);
}
}   // namespace

int main() {
    Kvasir::Test::test(
      "KVASIR_DETAIL_CALLER_PC: the caller's return address, 32 bits, on a 64-bit host");
    callerPcCompilesOnTheHost();
#ifndef KVASIR_TEST_NO_LONGJMP
    Kvasir::Test::test(
      "KVASIR_DETAIL_FATAL_AT with a line: start, line, raiseAt(cause, pc, detail)");
    fatalAtWithALine();
    Kvasir::Test::test("KVASIR_DETAIL_FATAL_AT without a line: start, raiseAt");
    fatalAtWithoutALine();
    Kvasir::Test::test("KVASIR_DETAIL_FATAL: start, the statement, the end");
    fatalRunsStartStatementEnd();
    Kvasir::Test::test("KVASIR_PANIC: start, line, raise(user)");
    panicMacro();
    Kvasir::Test::test("a library end's site is its caller");
    siteIsTheCaller();
#endif
    return Kvasir::Test::report();
}
