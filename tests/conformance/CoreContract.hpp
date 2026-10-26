#pragma once
// The core contract: what a core package (core_cortex_*) offers the SDK, written over today's names. Test support,
// not library API: a core's conformance TU (core_cortex_<x>/tests/conformance/conformance.cpp) and every chip's TU
// (through ChipContract.hpp) include the core first, then this header, and state each row as REQUIRE or GAP.
//
//   KVASIR_CORE_CONFORMANCE_REQUIRE("Core::Debug::isDebuggerConnected()",
//                                   Kvasir::Conformance::Row::coreDebugIsDebuggerConnected);
//
// A GAP row that the core fills fails the build: the row then moves to REQUIRE. Names that may not exist are asked
// through stand-ins in namespace `Absent`, nominated by a using-directive in the contract namespace: qualified lookup
// finds the core's own declaration first and the stand-in (of type `Absent::Missing`, or a type of that name in
// `Absent`) only when there is none ([namespace.qual]). Include this header LAST in a TU.

#include <type_traits>

namespace Kvasir::Conformance::Absent {
struct Missing {};

// Core
Missing haltingDebug();
// Core::Debug
Missing isDebuggerConnected();

// Startup::Core
struct SecondaryCoreTraits : Missing {};
}   // namespace Kvasir::Conformance::Absent

namespace Kvasir::Core { using namespace Kvasir::Conformance::Absent; }   // namespace Kvasir::Core

namespace Kvasir::Core::Debug {
using namespace Kvasir::Conformance::Absent;
}   // namespace Kvasir::Core::Debug

namespace Kvasir::Startup::Core {
using namespace Kvasir::Conformance::Absent;
}   // namespace Kvasir::Startup::Core

namespace Kvasir::Conformance {
template<typename T>
inline constexpr bool present = !std::is_same_v<std::remove_cvref_t<T>, Absent::Missing>;

namespace Row {
    // "is halting debug on" under one name for every core
    inline constexpr bool coreHaltingDebug = present<decltype(Kvasir::Core::haltingDebug())>;
    // today's spelling of it, on the cores that can read DHCSR themselves (not the M0+)
    inline constexpr bool coreDebugIsDebuggerConnected
      = std::is_same_v<decltype(Kvasir::Core::Debug::isDebuggerConnected()), bool>;
    // what a secondary core does for itself before compiled code runs on it (the entry trampoline's CPACR mask)
    inline constexpr bool coreSecondaryCoreTraits
      = !std::is_same_v<Kvasir::Startup::Core::SecondaryCoreTraits, Absent::SecondaryCoreTraits>;
}   // namespace Row
}   // namespace Kvasir::Conformance

#define KVASIR_CORE_CONFORMANCE_REQUIRE(row, ...)                  \
    static_assert((__VA_ARGS__), "core contract: " row " missing")
#define KVASIR_CORE_CONFORMANCE_GAP(row, ...)                                      \
    static_assert(!(__VA_ARGS__),                                                  \
                  "core contract: " row                                            \
                  " exists now - move it to the required list (CoreContract.hpp)")
