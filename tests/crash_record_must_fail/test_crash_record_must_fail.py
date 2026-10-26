#!/usr/bin/env python3
"""An injected CrashRecord format (Runtime/CrashRecord.hpp) whose part hands back another shape than the record
itself must not compile where the part is used: a Panic part whose take() returns anything but
std::optional<Panic::FullRecord> fails takeLastPanic() with PanicPart's message, a Fault part whose take() returns
anything but std::optional<Fault::FullRecord> fails takeLastFault() with FaultPart's. The parts of Full<> compile.

    test_crash_record_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = """#include "kvasir/Runtime/FaultRecord.hpp"
#include "kvasir/Runtime/Panic.hpp"
#include <cstdint>
#include <optional>
struct Brief { std::uint32_t count, cause, pc, detail; };   // a view beside the record
struct PanicTakesBrief {
    static bool recorded() { return false; }
    static std::optional<std::uint32_t> cause() { return std::nullopt; }
    static void record(Kvasir::Panic::Info const&) {}
    static std::optional<Brief> take() { return std::nullopt; }
};
struct FaultTakesBrief {
    static constexpr bool clearOnPowerOn = true;
    static bool recorded() { return false; }
    static std::optional<Brief> take() { return std::nullopt; }
};
struct MyFormat {
    static constexpr Kvasir::CrashRecord::Options options{};
    using Panic = %s;
    using Fault = %s;
};
template<> struct Kvasir::CrashRecord::RecordFormat<Kvasir::Tag::User> { using type = ::MyFormat; };
"""

FULL_PANIC = "Kvasir::CrashRecord::Full<>::Panic"
FULL_FAULT = "Kvasir::CrashRecord::Full<>::Fault"
USE = "int main() { return Kvasir::Panic::takeLastPanic().has_value() + Kvasir::Fault::takeLastFault().has_value(); }\n"

# (name, Panic part, Fault part, the wanted message; None: must compile)
ROWS = [
    ("control: Full<>'s parts (want None)", FULL_PANIC, FULL_FAULT, None),
    ("a Panic part whose take() returns a brief view", "PanicTakesBrief", FULL_FAULT,
     "the injected CrashRecord format's Panic part lacks"),
    ("a Fault part whose take() returns a brief view", FULL_PANIC, "FaultTakesBrief",
     "the injected CrashRecord format's Fault part lacks"),
]

CASES = [Case(name, USE, None if want is None else [want], head=HEAD % (panic, fault))
         for name, panic, fault, want in ROWS]

if __name__ == "__main__":
    sys.exit(run("", CASES))
