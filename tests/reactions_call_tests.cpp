// The Call<&fn> reactions (kvasir/Port/Hooks.hpp's table), each through its component's late hook in one program: a
// boot guard at its limit calls fn with the report; a starved health check calls fn with what starved, and the
// watchdog then stops being fed (KeepFeeding false, the default); a failed soft check calls fn after its line and
// returns false; a register wait that times out calls fn with the register's address and hands back the WaitResult.
// None of it starts a fatal path on its own: the image's fatal start counts, and stays at 0.
#include "kvasir/Port/ResetKind.hpp"

namespace Kvasir::PM {
inline ResetKind fakeReset = ResetKind::powerOn;

inline ResetKind resetKind() { return fakeReset; }
}   // namespace Kvasir::PM

#include "fixtures/Health.hpp"
#include "kvasir/Register/Wait.hpp"
#include "kvasir/Runtime/BootGuard.hpp"
#include "kvasir/Runtime/Check.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"
#include "test_registers.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

using namespace std::chrono_literals;

namespace {
int fatalStarts = 0;

struct CountingFatalStart {
    static void operator()() { ++fatalStarts; }
};

int                                      limitCalls = 0;
std::optional<Kvasir::BootGuard::Report> limitReport;

void onLimit(Kvasir::BootGuard::Report const& r) {
    ++limitCalls;
    limitReport = r;
}

int                                    starvedCalls = 0;
std::optional<Kvasir::Health::Starved> starved;

void onStarved(Kvasir::Health::Starved const& s) {
    ++starvedCalls;
    starved = s;
}

int softCalls = 0;

void onSoftFailure() { ++softCalls; }

int           timeoutCalls = 0;
std::uint32_t timeoutAddress{};

void onTimeout(std::uint32_t address) {
    ++timeoutCalls;
    timeoutAddress = address;
}

template<typename Tag>
struct Loop {
    static constexpr std::string_view name        = "loop";
    static constexpr auto             maxInterval = 50ms;
};

struct GuardConfig {
    static constexpr std::uint16_t limit        = 1;
    static constexpr auto          healthyAfter = 30s;
};

using GuardClock = Kvasir::Test::FakeClockT<std::chrono::milliseconds>;
using Guard      = Kvasir::BootGuard::Guard<GuardConfig, GuardClock>;
}   // namespace

template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = CountingFatalStart;
};

template<>
struct Kvasir::BootGuard::LimitReaction<Kvasir::Tag::User> {
    using type = Kvasir::BootGuard::OnLimit::Call<&onLimit>;
};

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Health::OnStarvation::Call<&onStarved>;
};

template<>
struct Kvasir::Check::SoftFailureReaction<Kvasir::Tag::User> {
    using type = Kvasir::Check::OnSoftFailure::Call<&onSoftFailure>;
};

template<>
struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> {
    using type = Kvasir::Register::OnTimeout::Call<&onTimeout>;
};

int main() {
    using Kvasir::Test::test;
    using namespace Kvasir::Test::HealthFixture;
    namespace H = Kvasir::Health;

    test("BootGuard OnLimit::Call: fn with the report at the limit, not before");
    {
        Kvasir::BootGuard::state.clear();
        Kvasir::Panic::lastPanic.clear();
        Kvasir::Fault::lastFault.clear();
        Kvasir::PM::fakeReset = Kvasir::ResetKind::powerOn;
        Guard::runtimeInit();
        CHECK_EQ(limitCalls, 0);
        // the run ends in a fault, the next boot is a software reset: one failed run = the limit
        Kvasir::Fault::lastFault.update([](Kvasir::Fault::FullRecord& f) {
            f.count = 1;
            f.pc    = 0x1000'0100U;
        });
        Kvasir::PM::fakeReset = Kvasir::ResetKind::softwareRequest;
        Guard::runtimeInit();
        CHECK_EQ(limitCalls, 1);
        CHECK(limitReport && limitReport->atLimit() && limitReport->state.consecutive == 1);
        CHECK_EQ(fatalStarts, 0);
    }

    test(
      "Health OnStarvation::Call: fn with the starved check, then the watchdog is no longer fed");
    {
        struct T {};

        using S = Sup<T, Loop<T>>;
        static_assert(!H::OnStarvation::Call<&onStarved>::keepFeeding);
        static_assert(H::OnStarvation::Call<&onStarved, true>::keepFeeding);
        S::service();
        turns<S>(10);
        CHECK_EQ(starvedCalls, 0);
        turns<S>(1);   // grace 50 + window 50: starved on the 11th turn
        CHECK_EQ(starvedCalls, 1);
        CHECK(starved && starved->name == "loop" && starved->silentMs == 110
              && starved->allowedMs == 100);
        int const fed = Watchdog<T>::feeds;
        turns<S>(5);
        CHECK_EQ(Watchdog<T>::feeds, fed);   // latched: the watchdog resets the chip
        CHECK_EQ(starvedCalls, 1);
        CHECK_EQ(fatalStarts, 0);
    }

    test("Check OnSoftFailure::Call: fn after the line, the soft check returns false");
    {
        int const v = 3;
        CHECK(KVASIR_SOFT_CHECK(v < 5));
        CHECK_EQ(softCalls, 0);
        CHECK(!KVASIR_SOFT_CHECK(v > 5, "v {}", v));
        CHECK_EQ(softCalls, 1);
        CHECK_EQ(fatalStarts, 0);
    }

    test("Register OnTimeout::Call: fn with the polled register's address, then the WaitResult");
    {
        using namespace Kvasir::Register;
        auto& m      = Kvasir::Test::recorder.model(CtrlReg::Addr::value);
        m.onRead     = [](unsigned) { return 0U; };   // never true
        auto const r = waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
        CHECK(r.timedOut && r.polls == 3);
        CHECK_EQ(timeoutCalls, 1);
        CHECK_EQ(timeoutAddress, static_cast<std::uint32_t>(CtrlReg::Addr::value));
        int        polls = 0;
        auto const c     = waitUntil<Bound::Polls<2>>([&] { return ++polls > 5; });
        CHECK(c.timedOut && timeoutCalls == 2 && timeoutAddress == 0U);
        CHECK_EQ(fatalStarts, 0);
    }
    return Kvasir::Test::report();
}
