#pragma once
// The chip contract: what a chip package offers the SDK and the drivers above it, written over today's names. Test
// support, not library API: a chip's conformance TU (chip_<x>/tests/conformance/conformance.cpp) includes the chip
// first, then this header, and states every row as REQUIRE (the chip has it) or GAP (the chip does not have it yet).
//
//   KVASIR_CONFORMANCE_REQUIRE("PM::resetKind()", Kvasir::Conformance::Row::resetKind);
//   KVASIR_CONFORMANCE_GAP("Chip::reboot()", Kvasir::Conformance::Row::chipReboot);
//
// A GAP row that the chip fills fails the build and says so: the row then moves to REQUIRE, and the list can only
// grow. A rename of a contract name changes this header and the chips' TUs together.
//
// How a row asks for a name that may not exist without a hard error: this header puts a using-directive for the
// namespace `Absent` into each contract namespace (Kvasir, Kvasir::PM, Kvasir::Chip here; Kvasir::Core, Kvasir::Core::Debug in
// CoreContract.hpp, which this header includes).
// Qualified lookup of `Kvasir::PM::resetKind` finds the chip's own declaration when there is one and looks into the
// nominated namespace only when there is none ([namespace.qual]) - there it finds a stand-in whose type is
// `Absent::Missing`. So include this header LAST in a TU: names declared after it are not seen by the rows.

#include "CoreContract.hpp"
#include "kvasir/Health/HealthKey.hpp"
#include "kvasir/Port/ResetKind.hpp"
#include "kvasir/Register/Wait.hpp"
#include "kvasir/Runtime/CellStore.hpp"
#include "kvasir/Runtime/ImageCheck.hpp"
#include "kvasir/Util/StaticString.hpp"

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Kvasir::Conformance::Absent {

// PM
Missing resetCause();
Missing resetKind();
// Chip
Missing                  reboot();
Missing                  debuggerPresent();
inline constexpr Missing part{};
inline constexpr Missing partName{};

// Kvasir (the watchdog every chip names Kvasir::Watchdog<Config>; the chip's serial number)
template<typename...>
struct Watchdog : Missing {};

Missing serialNumber();
Missing serialNumberString();
}   // namespace Kvasir::Conformance::Absent

namespace Kvasir::PM { using namespace Kvasir::Conformance::Absent; }   // namespace Kvasir::PM

namespace Kvasir::Chip { using namespace Kvasir::Conformance::Absent; }   // namespace Kvasir::Chip

namespace Kvasir { using namespace Kvasir::Conformance::Absent; }   // namespace Kvasir

namespace Kvasir::Conformance {
// A watchdog the health supervisor can drive (Health/HealthKey.hpp WatchdogShape: feed and Timeout) with a gate: feed
// and arm take its FeedKey. The supervisor takes disarm as optional (the STM32 IWDG has none), so the contract does
// too.
template<typename W>
concept HealthWatchdog = Kvasir::Health::WatchdogShape<W> && requires(Kvasir::Health::FeedKey key) {
    W::feed(key);
    W::arm(key);
};

namespace Row {
    // the reset cause in the chip's own terms, and as the boot guard sees it
    inline constexpr bool resetCause = present<decltype(Kvasir::PM::resetCause())>;
    inline constexpr bool resetKind
      = std::is_same_v<decltype(Kvasir::PM::resetKind()), Kvasir::ResetKind>;
    // the fastest core clock the chip is rated for, for the wait bounds (Register::MicrosecondsBound): the chip's
    // specialisation of the hook (chip/WaitBounds.hpp); the primary's 0 is "none"
    inline constexpr bool cpuHzCeiling
      = Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User>::value > 0;
    // a reboot that every chip spells the same, and "is a debugger attached"
    inline constexpr bool chipReboot          = present<decltype(Kvasir::Chip::reboot())>;
    inline constexpr bool chipDebuggerPresent = present<decltype(Kvasir::Chip::debuggerPresent())>;
    // a chip watchdog under the name Kvasir::Watchdog<Config> (named, not instantiated); its shape for the health
    // supervisor is HealthWatchdog<the chip's watchdog type>
    inline constexpr bool kvasirWatchdog
      = !std::is_same_v<Kvasir::Watchdog<void>, Absent::Watchdog<void>>;
    // the chip's serial number as bytes (a std::array<std::byte, N>) and as upper-case hex text, two digits a byte (a
    // Kvasir::StaticString<2 * N>, Util/HexString.hpp's): the shape, not only the names
    template<typename Bytes, typename Text>
    inline constexpr bool serialNumberShape = false;
    template<std::size_t N>
    inline constexpr bool serialNumberShape<std::array<std::byte, N>, Kvasir::StaticString<2 * N>>
      = N != 0;
    inline constexpr bool serialNumber
      = serialNumberShape<std::remove_cvref_t<decltype(Kvasir::serialNumber())>,
                          std::remove_cvref_t<decltype(Kvasir::serialNumberString())>>;
    // the part as a constant (which die, which package: Chip::part, of the type Chip::Part) and as text (partName)
    inline constexpr bool chipPart
      = present<decltype(Kvasir::Chip::part)> && present<decltype(Kvasir::Chip::partName)>;
}   // namespace Row
}   // namespace Kvasir::Conformance

// the condition is a constant expression, usually a Kvasir::Conformance::Row value or a concept check of a chip type
#define KVASIR_CONFORMANCE_REQUIRE(row, ...)                       \
    static_assert((__VA_ARGS__), "chip contract: " row " missing")
#define KVASIR_CONFORMANCE_GAP(row, ...)                                           \
    static_assert(!(__VA_ARGS__),                                                  \
                  "chip contract: " row                                            \
                  " exists now - move it to the required list (ChipContract.hpp)")
