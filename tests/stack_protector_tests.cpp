// Kvasir::StackProtector (Runtime/StackProtector.hpp): an intact sentinel lets turn() return idle; an
// overwritten one is a panic Cause::stackOverflow with no program site (pc 0) and the overwritten words as its detail;
// runtimeInit() heals it. The panic leaves by longjmp (harness/PanicCatcher.hpp).
#include "harness/PanicCatcher.hpp"
#include "kvasir/Runtime/StackProtector.hpp"
#include "test_harness.hpp"

template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::CatchPanic;
};

namespace {
using Kvasir::Panic::Cause;
using P32 = Kvasir::StackProtector<>;
using P64 = Kvasir::StackProtector<64>;

template<typename P>
std::optional<Kvasir::Panic::Info> check() {
    return Kvasir::Test::panicOf([] { P::turn(); });
}

void intact() {
    Kvasir::Test::test("an intact sentinel: turn() returns idle");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    P32::runtimeInit();
    CHECK(!check<P32>().has_value());
    CHECK(P32::turn() == Kvasir::Turn::idle());
}

void broken() {
    Kvasir::Test::test("one word overwritten: stackOverflow, pc 0, detail 1");
#ifdef KVASIR_TEST_NO_LONGJMP
    return;
#endif
    P32::runtimeInit();
    P32::sentinel.back() = 0x5A5A'5A5AU;
    auto const one       = check<P32>();
    CHECK(one && one->cause == Cause::stackOverflow && one->pc == 0U && one->detail == 1U);

    Kvasir::Test::test("all eight overwritten: detail 8; runtimeInit() heals it");
    P32::sentinel.fill(0);
    auto const all = check<P32>();
    CHECK(all && all->cause == Cause::stackOverflow && all->detail == 8U);
    P32::runtimeInit();
    CHECK(!check<P32>().has_value());

    Kvasir::Test::test("StackProtector<64>: up to 16 words");
    P64::runtimeInit();
    P64::sentinel.fill(1);
    auto const wide = check<P64>();
    CHECK(wide && wide->cause == Cause::stackOverflow && wide->detail == 16U);
}

static_assert(Kvasir::Panic::name(Cause::stackOverflow) == std::string_view{"stack overflow"});
}   // namespace

int main() {
    intact();
    broken();
    return Kvasir::Test::report();
}
