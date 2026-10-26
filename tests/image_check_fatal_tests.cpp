// Kvasir::ImageCheck's mismatch policies on a fatal path or not (Runtime/ImageCheck.hpp): OnMismatch::Log writes its line
// and goes on - no Panic::beginFatal() (the outputs stay on, the log does not wait for the host from then on);
// OnMismatch::PanicAfter<N> starts the fatal path before the line of its N-th corrupt pass in a row, then raises, and
// on the passes before (or with a debugger) only logs. Built with USE_UC_LOG against support/uc_log_stub, which records
// the level of each line; the injected fatal start records 'S', the injected handler 'R'.
#include "kvasir/Runtime/ImageCheck.hpp"
#include "test_harness.hpp"

#include <csetjmp>
#include <cstdint>
#include <string>

static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

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

namespace {
std::jmp_buf back;

struct RecordStart {
    static void operator()() { stub::events += 'S'; }
};

struct RecordRaise {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const&) {
        stub::events += 'R';
        std::longjmp(back, 1);
    }
};

struct NoDebugger {
    static bool attached() { return false; }
};

struct Debugged {
    static bool attached() { return true; }
};

template<typename Policy>
std::string pass(std::uint32_t inARow) {
    stub::events.clear();
    if(setjmp(back) == 0) { Policy::mismatch(1U, 2U, inARow); }
    return stub::events;
}
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
void logIsNotFatal() {
    stub::events.clear();
    IC::OnMismatch::Log::mismatch(1U, 2U);
    CHECK(stub::events == "E");   // an error line: the run goes on (Runtime/Check.hpp's classes)
}
}   // namespace

// TSan's longjmp interceptor aborts on a jump out of a function that does not return (harness/NoLongjmp.hpp)
#include "harness/NoLongjmp.hpp"

namespace {
[[maybe_unused]] void panicAfterStartsTheFatalPathFirst() {
    using P = IC::OnMismatch::PanicAfter<2, NoDebugger>;
    CHECK(pass<P>(1) == "E");   // a pass before the limit: an error line
    CHECK(pass<P>(2)
          == "SCSR");   // the fatal pass: start, crit line, (the dispatch's start), raise
    using D = IC::OnMismatch::PanicAfter<2, Debugged>;
    CHECK(pass<D>(2) == "E");   // under a debugger it goes on
}
}   // namespace

int main() {
    Kvasir::Test::test("OnMismatch::Log: its line only, no fatal start");
    logIsNotFatal();
#ifndef KVASIR_TEST_NO_LONGJMP
    Kvasir::Test::test("OnMismatch::PanicAfter: start, line, raise on the fatal pass only");
    panicAfterStartsTheFatalPathFirst();
#endif
    return Kvasir::Test::report();
}
