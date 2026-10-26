// Ignore, the register wait's Log, the boot guard's Count and the Call<&fn> of a panic and a fault (Port/Hooks.hpp's
// table), each through its component's late hook in one program, with the stub uc_log (support/uc_log_stub records each
// line's level): Ignore writes nothing of its own and the run goes on (a starved check is fed and gets a fresh window,
// a soft check returns false, a corrupt pass stays corrupt); OnTimeout::Log only stores in the wait and writes one
// warning per batch from Register::TimeoutReport's main-loop turn; OnLimit::Count is 1 when the run starts at the
// limit; OnPanic::Call records, then calls fn; OnFault::Call calls fn. No fatal start runs but the panic's own.
#include "kvasir/Port/ResetKind.hpp"

namespace Kvasir::PM {
inline ResetKind fakeReset = ResetKind::powerOn;

inline ResetKind resetKind() { return fakeReset; }
}   // namespace Kvasir::PM

#include "fixtures/Health.hpp"
#include "harness/PanicCatcher.hpp"
#include "kvasir/Register/Wait.hpp"
#include "kvasir/Register/WaitLog.hpp"
#include "kvasir/Runtime/BootGuard.hpp"
#include "kvasir/Runtime/Check.hpp"
#include "kvasir/Runtime/FaultReaction.hpp"
#include "kvasir/Runtime/ImageCheck.hpp"
#include "support/FakeClock.hpp"
#include "test_harness.hpp"
#include "test_registers.hpp"
#include "uc_log/uc_log.hpp"   // the stub

#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <optional>
#include <string_view>

static_assert(stub::isStub,
              "the uc_log stub, not uc_log's own header");

using namespace std::chrono_literals;

namespace {
int fatalStarts = 0;

struct CountingFatalStart {
    static void operator()() { ++fatalStarts; }
};

std::optional<Kvasir::Panic::Info> called;

[[noreturn]] void onPanic(Kvasir::Panic::Info const& info) {
    called = info;
    std::longjmp(Kvasir::Test::panicBack, 1);
}

#ifndef KVASIR_TEST_NO_LONGJMP
int faultCalls = 0;

[[noreturn]] void onFault() {
    ++faultCalls;
    std::longjmp(Kvasir::Test::panicBack, 2);
}
#endif

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
}   // namespace

template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = CountingFatalStart;
};

template<>
struct Kvasir::Panic::PanicReaction<Kvasir::Tag::User> {
    using type = Kvasir::Panic::OnPanic::Call<&onPanic>;
};

template<>
struct Kvasir::Register::WaitPolicy<Kvasir::Tag::User> {
    using type = Kvasir::Register::OnTimeout::Log;
};

template<>
struct Kvasir::Health::StarvationReaction<Kvasir::Tag::User> {
    using type = Kvasir::Health::OnStarvation::Ignore;
};

template<>
struct Kvasir::Check::SoftFailureReaction<Kvasir::Tag::User> {
    using type = Kvasir::Check::OnSoftFailure::Ignore;
};

template<>
struct Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User> {
    using type = Kvasir::ImageCheck::OnMismatch::Ignore;
};

template<>
struct Kvasir::BootGuard::LimitReaction<Kvasir::Tag::User> {
    using type = Kvasir::BootGuard::OnLimit::Count;
};

int main() {
    using Kvasir::Test::test;
    using namespace Kvasir::Test::HealthFixture;
    namespace H = Kvasir::Health;

    test(
      "OnTimeout::Log: the wait only stores and counts; one warning per batch, from its main-loop "
      "turn");
    {
        using namespace Kvasir::Register;
        using L  = OnTimeout::Log;
        auto& m  = Kvasir::Test::recorder.model(CtrlReg::Addr::value);
        m.onRead = [](unsigned) { return 0U; };   // never true
        stub::events.clear();
        auto const a = waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
        auto const b = waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
        CHECK(a.timedOut && b.timedOut);
        CHECK_EQ(L::timeouts, 2U);
        CHECK_EQ(L::lastAddress, static_cast<std::uint32_t>(CtrlReg::Addr::value));
        CHECK(stub::events.empty());   // nothing from inside the wait
        using R = Kvasir::Register::TimeoutReport;
        R::turn();
        R::turn();   // nothing new: no second line
        CHECK(stub::events == "W");
        (void)waitUntil<Bound::Polls<3>>(isSet(CtrlReg::en));
        R::turn();
        CHECK(stub::events == "WW");
        static_assert(std::is_same_v<R::Extends::Hook, Kvasir::Hook::MainLoop>);
    }

    test("OnStarvation::Ignore: nothing of its own, the watchdog fed, the record still written");
    {
        struct T {};

        using S = Sup<T, Loop<T>>;
        H::lastStarvation.clear();
        S::service();
        turns<S>(11);   // grace 50 + window 50: starved on the 11th turn
        CHECK(H::lastStarvation.load().has_value());
        CHECK_EQ(Watchdog<T>::feeds, 12);
        stub::events.clear();
        turns<S>(11);   // a fresh window, starved again, still fed, no line from the reaction
        CHECK_EQ(Watchdog<T>::feeds, 23);
        CHECK_EQ(fatalStarts, 0);
    }

    test("OnSoftFailure::Ignore: the soft check's own line, false, the run goes on");
    {
        // what a failed KVASIR_SOFT_CHECK calls after its line (the macro needs uc_log's catalog, not the stub)
        stub::events.clear();
        CHECK(!Kvasir::Check::Detail::softCheckFailed([] { UC_LOG_E("soft check"); }));
        CHECK(stub::events == "E");
        CHECK_EQ(fatalStarts, 0);
    }

    test("OnMismatch::Ignore: no line");
    {
        using R = Kvasir::ImageCheck::MismatchReaction<Kvasir::Tag::User>::type;
        stub::events.clear();
        R::mismatch(1U, 2U);
        CHECK(stub::events.empty());
    }

    test("OnLimit::Count: 0 below the limit, 1 when the run starts at it");
    {
        Kvasir::BootGuard::state.clear();
        Kvasir::Panic::lastPanic.clear();
        Kvasir::Fault::lastFault.clear();
        Kvasir::PM::fakeReset = Kvasir::ResetKind::powerOn;
        Guard::runtimeInit();
        CHECK_EQ(Kvasir::BootGuard::OnLimit::Count::limits, 0U);
        Kvasir::Fault::lastFault.update([](Kvasir::Fault::FullRecord& f) {
            f.count = 1;
            f.pc    = 0x1000'0100U;
        });
        Kvasir::PM::fakeReset = Kvasir::ResetKind::softwareRequest;
        Guard::runtimeInit();
        CHECK_EQ(Kvasir::BootGuard::OnLimit::Count::limits, 1U);
    }

#ifndef KVASIR_TEST_NO_LONGJMP
    test("OnPanic::Call: recorded first, then fn with the Info");
    {
        Kvasir::Panic::lastPanic.clear();
        Kvasir::Panic::Detail::entered = false;
        called.reset();
        if(setjmp(Kvasir::Test::panicBack) == 0) {
            Kvasir::Panic::raiseAt(Kvasir::Panic::Cause::user, 0x1234, 7);
        }
        CHECK(called && called->cause == Kvasir::Panic::Cause::user && called->detail == 7U);
        CHECK(Kvasir::Panic::lastPanic.valid());
    }

    test("OnFault::Call: fn");
    {
        if(setjmp(Kvasir::Test::panicBack) == 0) { Kvasir::Fault::OnFault::Call<&onFault>{}(); }
        CHECK_EQ(faultCalls, 1);
    }
#endif
    return Kvasir::Test::report();
}
