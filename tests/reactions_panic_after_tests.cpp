// The PanicAfter<N> reactions (Port/Hooks.hpp's table), each through its component's late hook in one program, with
// the stub uc_log (support/uc_log_stub records each line's level) and the panic caught by longjmp
// (harness/PanicCatcher): what "N in a row" counts for each - a register wait's timeouts with no wait holding between
// them, a supervisor's starvations of this run, a soft check's failures of this run, the boot guard's boots that start
// at its limit - the events before the N-th go on, the N-th panics with the component's cause.
#include "kvasir/Port/ResetKind.hpp"

namespace Kvasir::PM {
inline ResetKind fakeReset = ResetKind::powerOn;

inline ResetKind resetKind() { return fakeReset; }
}   // namespace Kvasir::PM

#include "fixtures/Health.hpp"
#include "harness/PanicCatcher.hpp"
#include "kvasir/Register/Wait.hpp"
#include "kvasir/Runtime/BootGuard.hpp"
#include "kvasir/Runtime/Check.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"
#include "test_registers.hpp"
#include "uc_log/uc_log.hpp"   // the stub

#include <chrono>
#include <cstdint>
#include <string_view>

static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

using namespace std::chrono_literals;

#ifndef KVASIR_TEST_NO_LONGJMP   // every case leaves its panic by longjmp: nothing to run under TSan
namespace {
template<typename Tag>
struct Loop {
    static constexpr std::string_view name        = "loop";
    static constexpr auto             maxInterval = 50ms;
};

struct GuardConfig {
    static constexpr std::uint16_t limit        = 1;
    static constexpr auto          healthyAfter = 30s;
};

using Guard
  = Kvasir::BootGuard::Guard<GuardConfig, Kvasir::Test::FakeClockT<std::chrono::milliseconds>>;

void failedRun() {
    Kvasir::Fault::lastFault.update([](Kvasir::Fault::FullRecord& f) {
        f.count = 1;
        f.pc    = 0x1000'0100U;
    });
    Kvasir::PM::fakeReset = Kvasir::ResetKind::softwareRequest;
}
}   // namespace
#endif

template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Test::CatchPanic;
};

template<>
struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> {
    using type = Kvasir::Register::OnTimeout::PanicAfter<3>;
};

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Health::OnStarvation::PanicAfter<2>;
};

template<>
struct Kvasir::Check::SoftFailureReaction<Kvasir::Tag::User> {
    using type = Kvasir::Check::OnSoftFailure::PanicAfter<2>;
};

template<>
struct Kvasir::BootGuard::LimitReaction<Kvasir::Tag::User> {
    using type = Kvasir::BootGuard::OnLimit::PanicAfter<2>;
};

int main() {
    using Kvasir::Panic::Cause;
    using Kvasir::Test::test;
#ifndef KVASIR_TEST_NO_LONGJMP
    using Kvasir::Test::panicOf;
    using namespace Kvasir::Test::HealthFixture;

    test("OnTimeout::PanicAfter<3>: timeouts in a row, a wait that holds starts the count again");
    {
        using namespace Kvasir::Register;
        auto& m     = Kvasir::Test::recorder.model(CtrlReg::Addr::value);
        bool  true_ = false;
        m.onRead    = [&true_](unsigned) { return true_ ? 1U : 0U; };
        auto wait   = [] { return waitUntil<Bound::Polls<2>>(isSet(CtrlReg::en)); };
        CHECK(!panicOf([&] { CHECK(wait().timedOut); }));
        CHECK(!panicOf([&] { CHECK(wait().timedOut); }));
        true_ = true;
        CHECK(!panicOf([&] { CHECK(!wait().timedOut); }));   // holds: the count starts again
        true_ = false;
        CHECK(!panicOf([&] { (void)wait(); }));
        CHECK(!panicOf([&] { (void)wait(); }));
        auto const p = panicOf([&] { (void)wait(); });   // the third in a row
        CHECK(p && p->cause == Cause::timeout
              && p->detail == static_cast<std::uint32_t>(CtrlReg::Addr::value));
    }

    test(
      "OnStarvation::PanicAfter<2>: the first starvation an error line and fed, the second the "
      "panic");
    {
        struct T {};

        using S = Sup<T, Loop<T>>;
        stub::events.clear();
        CHECK(!panicOf([] {
            S::service();
            turns<S>(11);
        }));
        CHECK_EQ(Kvasir::Health::OnStarvation::PanicAfter<2>::starvations, 1U);
        CHECK(stub::events.contains('E') && !stub::events.contains('C'));
        CHECK_EQ(Watchdog<T>::feeds, 12);
        auto const p = panicOf([] { turns<S>(11); });
        CHECK(p && p->cause == Cause::healthCheck && p->detail == 0U);
        CHECK(stub::events.contains('C'));
    }

    test("OnSoftFailure::PanicAfter<2>: the first failure false, the second the panic");
    {
        // what a failed KVASIR_SOFT_CHECK calls after its line (the macro needs uc_log's catalog, not the stub)
        auto const fail
          = [] { return Kvasir::Check::Detail::softCheckFailed([] { UC_LOG_E("soft check"); }); };
        CHECK(!panicOf([&] { CHECK(!fail()); }));
        auto const p = panicOf([&] { (void)fail(); });
        CHECK(p && p->cause == Cause::checkFailed);
    }

    test("OnLimit::PanicAfter<2>: the first boot at the limit goes on, the second in a row panics");
    {
        Kvasir::BootGuard::state.clear();
        Kvasir::Panic::lastPanic.clear();
        Kvasir::Fault::lastFault.clear();
        Kvasir::PM::fakeReset = Kvasir::ResetKind::powerOn;
        CHECK(!panicOf([] { Guard::runtimeInit(); }));
        failedRun();
        CHECK(!panicOf(
          [] { Guard::runtimeInit(); }));   // consecutive 1 = the limit: the first boot there
        failedRun();
        auto const p
          = panicOf([] { Guard::runtimeInit(); });   // consecutive 2: the second in a row
        CHECK(p && p->cause == Cause::bootLoop && p->detail == 2U);
    }
#endif
    return Kvasir::Test::report();
}
