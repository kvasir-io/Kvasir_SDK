// The Count reactions (kvasir/Port/Hooks.hpp's table), each through its component's late hook in one program: a
// health check that starves is counted and the watchdog stays fed (a fresh window each time it runs out again); a
// failed soft check is counted after its line and returns false; an image check mismatch is counted and the run goes
// on. Nothing of it starts a fatal path: the image's fatal start counts, and stays at 0.
#include "fixtures/Health.hpp"
#include "kvasir/Runtime/Check.hpp"
#include "kvasir/Runtime/ImageCheck.hpp"
#include "test_harness.hpp"

#include <chrono>
#include <string_view>
#include <type_traits>

using namespace std::chrono_literals;

namespace {
int fatalStarts = 0;

struct CountingFatalStart {
    static void operator()() { ++fatalStarts; }
};

template<typename Tag>
struct Loop {
    static constexpr std::string_view name        = "loop";
    static constexpr auto             maxInterval = 50ms;
};
}   // namespace

template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = CountingFatalStart;
};

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Health::OnStarvation::Count;
};

template<>
struct Kvasir::Check::SoftFailureReaction<Kvasir::Tag::User> {
    using type = Kvasir::Check::OnSoftFailure::Count;
};

template<>
struct Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User> {
    using type = Kvasir::ImageCheck::OnMismatch::Count;
};

int main() {
    using Kvasir::Test::test;
    using namespace Kvasir::Test::HealthFixture;
    namespace H = Kvasir::Health;

    test("Health OnStarvation::Count: counted, the watchdog stays fed, a fresh window each time");
    {
        struct T {};

        using S = Sup<T, Loop<T>>;
        static_assert(H::OnStarvation::Count::keepFeeding);
        S::service();
        turns<S>(11);   // grace 50 + window 50: starved on the 11th turn
        CHECK_EQ(H::OnStarvation::Count::starvations, 1U);
        CHECK_EQ(Watchdog<T>::feeds, 12);   // fed through it
        // a fresh window as long as the first (the check never beat: grace + maxInterval), still fed
        int const again
          = turnsUntil<S>(20, [] { return H::OnStarvation::Count::starvations == 2U; });
        CHECK_EQ(again, 11);
        CHECK_EQ(Watchdog<T>::feeds, 12 + again);
        CHECK_EQ(fatalStarts, 0);
    }

    test("Check OnSoftFailure::Count: the soft check returns false, counted, no panic");
    {
        int const v = 3;
        CHECK(KVASIR_SOFT_CHECK(v < 5, "fine"));
        CHECK_EQ(Kvasir::Check::OnSoftFailure::Count::failures, 0U);
        CHECK(!KVASIR_SOFT_CHECK(v > 5, "too small: {}", v));
        CHECK(!KVASIR_SOFT_CHECK(v > 6));
        CHECK_EQ(Kvasir::Check::OnSoftFailure::Count::failures, 2U);
        CHECK_EQ(fatalStarts, 0);
    }

    test("ImageCheck OnMismatch::Count: counted, no line, the run goes on");
    {
        using R = Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User>::type;
        static_assert(std::is_same_v<R, Kvasir::ImageCheck::OnMismatch::Count>);
        R::mismatch(0x1111'0000U, 0x2222'0000U);
        R::mismatch(0x1111'0001U, 0x2222'0000U);
        CHECK_EQ(Kvasir::ImageCheck::OnMismatch::Count::mismatches, 2U);
        CHECK_EQ(fatalStarts, 0);
    }
    return Kvasir::Test::report();
}
