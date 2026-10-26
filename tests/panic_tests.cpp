// Kvasir::Panic: the injected handler is the one raise() runs, with the cause and the caller's pc;
// a specialisation placed after raise()'s definition still wins; the record keeps the first panic
// and counts the rest; the dispatch runs the image's fatal start. (A panic inside the handler goes to
// Detail::stop(), here halt(), which is std::abort(): not run as a test.)
#include "kvasir/Runtime/Panic.hpp"

#include "harness/PanicCatcher.hpp"
#include "test_harness.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>

namespace {
using Kvasir::Test::panicBack;
using Kvasir::Test::panicSeen;
}   // namespace

// after raise() was defined and used: the dummy pack defers the lookup to the instantiation
template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::CatchPanic;
};

namespace {
int starts = 0;

struct CountStart {
    static void operator()() { ++starts; }
};
}   // namespace

// the image's fatal start: beginFatal() runs it, and the dispatch again after interrupts are masked
template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = CountStart;
};

namespace {
using Kvasir::Panic::Cause;

void startInDispatch() {
    Kvasir::Test::test(
      "a raise without beginFatal (a log-less site) runs the fatal start once, in the dispatch");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    starts = 0;
    CHECK(Kvasir::Test::panicOf([] { Kvasir::Panic::raise(Cause::bootLoop); }).has_value());
    CHECK(starts == 1);

    Kvasir::Test::test("beginFatal, then raise (a logged site): the fatal start runs twice");
    starts = 0;
    CHECK(Kvasir::Test::panicOf([] {
              Kvasir::Panic::beginFatal();
              Kvasir::Panic::raise(Cause::user);
          }).has_value());
    CHECK(starts == 2);
}

void handlerRuns() {
    Kvasir::Test::test("the injected handler runs, with the cause");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    panicSeen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(panicBack) == 0) { Kvasir::Panic::raise(Cause::stackSmash); }
    CHECK(panicSeen.has_value());
    CHECK(panicSeen && panicSeen->cause == Cause::stackSmash);
    CHECK(panicSeen && panicSeen->pc != 0U);
}

void recordKeepsFirst() {
    Kvasir::Test::test("record keeps the first panic and counts the rest");
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Panic::record({Cause::assertion, 0x1000});
    Kvasir::Panic::record({Cause::abort, 0x2000});
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r.has_value());
    CHECK(r && r->cause == static_cast<std::uint32_t>(Cause::assertion) && r->pc == 0x1000U
          && r->count == 2U);
    CHECK(!Kvasir::Panic::takeLastPanic().has_value());
}

void recordKeepsDetail() {
    Kvasir::Test::test("record keeps the site and the detail a library path gives raiseAt()");
    Kvasir::Panic::lastPanic.clear();
    Kvasir::Panic::record({Cause::unhandledInterrupt, 0, 40});
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->cause == static_cast<std::uint32_t>(Cause::unhandledInterrupt) && r->pc == 0U
          && r->detail == 40U);
}

static_assert(static_cast<unsigned char>(Cause::assertion) == 0,
              "the libc's assert header raises static_cast<Cause>(0)");
static_assert(Kvasir::Panic::Handler<Kvasir::Panic::OnPanic::Default>);
}   // namespace

int main() {
    handlerRuns();
    startInDispatch();
    recordKeepsFirst();
    recordKeepsDetail();
    return Kvasir::Test::report();
}
