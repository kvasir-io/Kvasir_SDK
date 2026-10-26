#!/usr/bin/env python3
"""Kvasir::Profile::LoopReport refuses a Period longer than half a wrap of its 32-bit time source (before, such a
report never fired and nothing said so): compile each snippet with -fsyntax-only, expect it to fail with the
message - and a period inside the bound, and one exactly at it, must compile.

    test_loop_profiler_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = """#include "kvasir/Runtime/LoopProfiler.hpp"
#include <chrono>
#include <cstdint>
#include <string_view>
struct TS150 {
    static constexpr std::uint64_t    ticksPerSecond = 150'000'000;
    static constexpr std::string_view name           = "fake";
    static std::uint32_t now() { return 0; }
};
struct TS1M {
    static constexpr std::uint64_t    ticksPerSecond = 1'000'000;
    static constexpr std::string_view name           = "fake";
    static std::uint32_t now() { return 0; }
};
struct Cfg { using TimeSource = TS150; };
struct CfgMicros { using TimeSource = TS1M; };
"""

MESSAGE = "half a wrap"


def report(cfg: str, period: str) -> str:
    return (f"constexpr auto P = {period};\n"
            f"using R = Kvasir::Profile::LoopReport<{cfg}, P>;\n"
            "using E = R::Extends;\n"
            "E const* e{};\n")


# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("period over half a wrap", report(
        "Cfg", "std::chrono::seconds{30}"), [MESSAGE]),
    Case("15 s at 150 MHz (2.25e9 ticks)", report(
        "Cfg", "std::chrono::seconds{15}"), [MESSAGE]),
    Case("one tick over 2^31", report("CfgMicros",
         "std::chrono::microseconds{2'147'483'649}"), [MESSAGE]),
    Case("10 s at 150 MHz compiles", report(
        "Cfg", "std::chrono::seconds{10}"), None),
    Case("exactly 2^31 ticks compiles", report("CfgMicros",
         "std::chrono::microseconds{2'147'483'648}"), None),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
