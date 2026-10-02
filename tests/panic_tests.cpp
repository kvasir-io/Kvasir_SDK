// Kvasir::Panic: the injected handler is the one raise() runs, with the cause and the caller's pc;
// a specialisation placed after raise()'s definition still wins; the record keeps the first panic
// and counts the rest. (A panic inside the handler goes to halt(), which is std::abort() here: not
// run as a test.)
#include "kvasir/Util/Panic.hpp"

#include "test_harness.hpp"

#include <csetjmp>
#include <cstdint>
#include <optional>

namespace Kvasir::Panic {
// on the target this is StartUp.hpp's; here the test's
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

namespace {
std::jmp_buf                       back;
std::optional<Kvasir::Panic::Info> seen;

struct TestHandler {
    [[noreturn]] static void operator()(Kvasir::Panic::Info const& info) {
        seen = info;
        std::longjmp(back, 1);
    }
};
}   // namespace

// after raise() was defined and used: the dummy pack defers the lookup to the instantiation
template<>
inline constexpr auto Kvasir::Panic::injectedHandler<> = TestHandler{};

namespace {
using Kvasir::Panic::Cause;

// TSan's longjmp interceptor aborts on a jump out of a function that does not return; this test
// has no threads for TSan to judge.
#if defined(__SANITIZE_THREAD__)
    #define PANIC_TEST_NO_LONGJMP 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define PANIC_TEST_NO_LONGJMP 1
    #endif
#endif

void handlerRuns() {
    Kvasir::Test::test("the injected handler runs, with the cause");
#ifdef PANIC_TEST_NO_LONGJMP
    return;
#endif
    seen.reset();
    Kvasir::Panic::Detail::entered = false;
    if(setjmp(back) == 0) { Kvasir::Panic::raise(Cause::stackSmash); }
    CHECK(seen.has_value());
    CHECK(seen && seen->cause == Cause::stackSmash);
    CHECK(seen && seen->pc != 0U);
}

void recordKeepsFirst() {
    Kvasir::Test::test("record keeps the first panic and counts the rest");
    Kvasir::Panic::lastPanic.magic = 0;
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
    Kvasir::Panic::lastPanic.magic = 0;
    Kvasir::Panic::record({Cause::unhandledInterrupt, 0, 40});
    auto const r = Kvasir::Panic::takeLastPanic();
    CHECK(r && r->cause == static_cast<std::uint32_t>(Cause::unhandledInterrupt) && r->pc == 0U
          && r->detail == 40U);
}

static_assert(static_cast<unsigned char>(Cause::assertion) == 0,
              "the libc's assert header raises static_cast<Cause>(0)");
static_assert(Kvasir::Panic::Handler<Kvasir::Panic::DefaultHandler>);
}   // namespace

int main() {
    handlerRuns();
    recordKeepsFirst();
    recordKeepsDetail();
    return Kvasir::Test::report();
}
