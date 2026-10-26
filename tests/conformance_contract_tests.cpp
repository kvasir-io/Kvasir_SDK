// The contract headers of the conformance trees (tests/conformance/ChipContract.hpp, CoreContract.hpp) on a fake chip
// that has every row: each REQUIRE holds, each GAP of a row the fake lacks holds, and a name the fake does not
// declare is `false`, not a hard error. The reverse directions (a REQUIRE of a missing row, a GAP of a present one)
// must not compile: tests/conformance_must_fail.
#include "kvasir/Health/HealthKey.hpp"
#include "kvasir/Port/ResetKind.hpp"
#include "kvasir/Register/Wait.hpp"

#include <chrono>
#include <cstdint>

// the fake chip: reset cause, CPU clock ceiling (the wait-bound hook), the core's debugger query
namespace Kvasir::PM {
enum class ResetCause : std::uint8_t { powerOn };

inline ResetCause resetCause() { return ResetCause::powerOn; }

inline Kvasir::ResetKind resetKind() { return Kvasir::ResetKind::powerOn; }
}   // namespace Kvasir::PM

template<>
struct Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User> {
    static constexpr std::uint64_t value = 100'000'000;
};

namespace Kvasir::Core::Debug {
inline bool isDebuggerConnected() { return false; }
}   // namespace Kvasir::Core::Debug

namespace Fake {
struct GatedWatchdog {
    static constexpr std::chrono::milliseconds timeout{100};

    static void feed(Kvasir::Health::FeedKey) {}

    static void arm(Kvasir::Health::FeedKey) {}

    static void disarm(Kvasir::Health::FeedKey) {}
};

struct PlainWatchdog {
    static constexpr std::chrono::milliseconds timeout{100};

    static void feed() {}

    static void arm() {}

    static void disarm() {}
};
}   // namespace Fake

#include "conformance/ChipContract.hpp"

KVASIR_CONFORMANCE_REQUIRE("PM::resetCause()",
                           Kvasir::Conformance::Row::resetCause);
KVASIR_CONFORMANCE_REQUIRE("PM::resetKind()",
                           Kvasir::Conformance::Row::resetKind);
KVASIR_CONFORMANCE_REQUIRE("Register::CpuHzCeiling<Tag::User>",
                           Kvasir::Conformance::Row::cpuHzCeiling);
KVASIR_CONFORMANCE_REQUIRE("health watchdog",
                           Kvasir::Conformance::HealthWatchdog<Fake::GatedWatchdog>);
KVASIR_CORE_CONFORMANCE_REQUIRE("Core::Debug::isDebuggerConnected()",
                                Kvasir::Conformance::Row::coreDebugIsDebuggerConnected);

KVASIR_CONFORMANCE_GAP("Chip::reboot()",
                       Kvasir::Conformance::Row::chipReboot);
KVASIR_CONFORMANCE_GAP("Chip::debuggerPresent()",
                       Kvasir::Conformance::Row::chipDebuggerPresent);
KVASIR_CONFORMANCE_GAP("Chip::Part",
                       Kvasir::Conformance::Row::chipPart);
KVASIR_CONFORMANCE_GAP("Kvasir::Watchdog",
                       Kvasir::Conformance::Row::kvasirWatchdog);
KVASIR_CONFORMANCE_GAP("Kvasir::serialNumber() / serialNumberString()",
                       Kvasir::Conformance::Row::serialNumber);
KVASIR_CONFORMANCE_GAP("plain watchdog",
                       Kvasir::Conformance::HealthWatchdog<Fake::PlainWatchdog>);
KVASIR_CORE_CONFORMANCE_GAP("Core::haltingDebug()",
                            Kvasir::Conformance::Row::coreHaltingDebug);
KVASIR_CORE_CONFORMANCE_GAP("Startup::Core::SecondaryCoreTraits",
                            Kvasir::Conformance::Row::coreSecondaryCoreTraits);

// the stand-ins do not hide the real names: the real ones are found, with their own types
static_assert(std::is_same_v<decltype(Kvasir::PM::resetCause()),
                             Kvasir::PM::ResetCause>);

int main() {
    // the real functions still resolve in evaluated code
    return Kvasir::PM::resetKind() == Kvasir::ResetKind::powerOn
            && !Kvasir::Core::Debug::isDebuggerConnected()
           ? 0
           : 1;
}
