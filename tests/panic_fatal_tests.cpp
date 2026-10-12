// Kvasir::Panic::beginFatal() with uc_log: it switches the user tag's log backend to its panic mode
// (uc_log::panicMode, LossPolicy::Counted), once per call, and does nothing for a backend without one. Built with
// USE_UC_LOG and uc_log's headers (nested here); the backend is a stand-in that counts.
#ifndef USE_UC_LOG
    #define USE_UC_LOG 1
#endif
#include "kvasir/Util/Panic.hpp"

#include "test_harness.hpp"

namespace {
int panicModes = 0;
int drains     = 0;
}   // namespace

// after Panic.hpp, as an application's HWConfig comes after the SDK headers it includes: the dummy pack in
// beginFatal() looks the backend up where it is used
template<>
struct uc_log::ComBackend<uc_log::Tag::User> {
    static void enterPanicMode() { ++panicModes; }

    static void drain() { ++drains; }
};

namespace {
struct OtherTag {};
}   // namespace

int main() {
    Kvasir::Test::test("beginFatal() puts the log backend into panic mode");
    Kvasir::Panic::beginFatal();
    CHECK(panicModes == 1);
    Kvasir::Panic::beginFatal();
    CHECK(panicModes == 2);

    Kvasir::Test::test("uc_log::drain() reaches the backend");
    uc_log::drain();
    CHECK(drains == 1);

    Kvasir::Test::test("a tag without a backend: nothing to do");
    uc_log::panicMode<OtherTag>();
    uc_log::drain<OtherTag>();
    CHECK(panicModes == 2 && drains == 1);
    return Kvasir::Test::report();
}
