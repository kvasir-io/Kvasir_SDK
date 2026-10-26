// Kvasir::Panic::beginFatal() with uc_log: it switches the user tag's log backend to its panic mode (uc_log::panicMode,
// LossPolicy::Counted), once per call, and does nothing for a backend without one. Built with USE_UC_LOG and uc_log's
// headers (nested here); the backend is a stand-in that counts. The image's Panic::FatalStartReaction runs first,
// before the backend's panic mode. OnPanic::Reset drains the log before the reboot.
#ifndef USE_UC_LOG
    #define USE_UC_LOG 1
#endif
#include "kvasir/Runtime/Panic.hpp"

#include "harness/NoLongjmp.hpp"   // TSan refuses a longjmp out of a noreturn path
#include "test_harness.hpp"

#include <csetjmp>
#include <cstdint>

namespace {
int panicModes = 0;
int drains     = 0;
int starts     = 0;
int modesAtStart{-1};   // panicModes when the start ran: it comes first

struct CountStart {
    static void operator()() {
        ++starts;
        modesAtStart = panicModes;
    }
};
}   // namespace

template<>
struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = CountStart;
};

// after Panic.hpp, as an application's HWConfig comes after the SDK headers it includes: the dummy pack in
// beginFatal() looks the backend up where it is used
template<>
struct uc_log::ComBackend<uc_log::Tag::User> {
    static void enterPanicMode() { ++panicModes; }

    static void drain() { ++drains; }

    static std::uint16_t bootsThatLogged() { return 7; }
};

namespace {
struct OtherTag {};

#ifndef KVASIR_TEST_NO_LONGJMP
std::jmp_buf back;
int          drainsAtReboot{-1};

struct FakeReboot {
    [[noreturn]] void operator()() const {
        drainsAtReboot = drains;
        std::longjmp(back, 1);
    }
};
#endif
}   // namespace

int main() {
    Kvasir::Test::test("beginFatal() puts the log backend into panic mode");
    Kvasir::Panic::beginFatal();
    CHECK(panicModes == 1);
    Kvasir::Panic::beginFatal();
    CHECK(panicModes == 2);

    Kvasir::Test::test("beginFatal() runs the injected start first, every time");
    CHECK(starts == 2 && modesAtStart == 1);

    Kvasir::Test::test("uc_log::drain() reaches the backend");
    uc_log::drain();
    CHECK(drains == 1);

    Kvasir::Test::test("a tag without a backend: nothing to do");
    uc_log::panicMode<OtherTag>();
    uc_log::drain<OtherTag>();
    CHECK(panicModes == 2 && drains == 1);

    Kvasir::Test::test(
      "uc_log::bootsThatLogged() is the backend's count; a tag without one has none");
    CHECK(uc_log::bootsThatLogged() == 7);
    static_assert(!uc_log::detail::HasBootCount<OtherTag>);

    Kvasir::Test::test("OnPanic::Reset: the record, the drain, then the reboot");
#ifndef KVASIR_TEST_NO_LONGJMP
    Kvasir::Panic::lastPanic.clear();
    if(setjmp(back) == 0) {
        Kvasir::Panic::OnPanic::Reset<FakeReboot>{}(
          Kvasir::Panic::Info{Kvasir::Panic::Cause::user, 0x10});
    }
    CHECK(drainsAtReboot == 2);
    CHECK(Kvasir::Panic::takeLastPanic().has_value());
#endif
    return Kvasir::Test::report();
}
