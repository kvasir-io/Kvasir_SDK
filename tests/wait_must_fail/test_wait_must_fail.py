#!/usr/bin/env python3
"""Kvasir::Register::MicrosecondsBound refuses to compile when no chip package specialised CpuHzCeiling<Tag::User>
(Register/Wait.hpp): compile each snippet with -fsyntax-only, expect the named message - and the same bound with
the specialisation in front of it must compile.

    test_wait_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Register/Wait.hpp"\n#include <cstdint>\n'
SPECIALISED = ("template<> struct Kvasir::Register::CpuHzCeiling<Kvasir::Tag::User> {\n"
               "    static constexpr std::uint64_t value = 48'000'000;\n};\n")
USE = "constexpr auto polls = Kvasir::Register::MicrosecondsBound<1'000>::polls;\n"

MESSAGE = "no chip package specialised Kvasir::Register::CpuHzCeiling<Tag::User>"

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("no CpuHzCeiling<Tag::User> specialisation", USE, [MESSAGE]),
    Case("specialised first compiles", SPECIALISED +
         USE + "static_assert(polls == 24'000);\n", None),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
