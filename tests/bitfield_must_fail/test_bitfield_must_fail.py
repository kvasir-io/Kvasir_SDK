#!/usr/bin/env python3
"""Kvasir::BitField refuses a field wider than 32 bits, or one with High below Low, with a message that says so
(before, it failed with a shift error inside Register::maskFromRange): compile each snippet with -fsyntax-only,
expect it to fail with the message - and a 32-bit field must still compile.

    test_bitfield_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Register/BitField.hpp"\n#include <array>\n#include <cstddef>\n'

MESSAGE = "at most 32 bits wide"

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("33 bits, default range",
         "Kvasir::BitField<32, 0> f{std::array<std::byte, 8>{}};\n", [MESSAGE]),
    Case("33 bits, explicit range",
         "Kvasir::BitField<32, 0, 0u, 5u> f{std::array<std::byte, 8>{}};\n", [MESSAGE]),
    Case("High below Low",
         "Kvasir::BitField<3, 5> f{std::array<std::byte, 8>{}};\n", [MESSAGE]),
    Case("32 bits compile",
         "Kvasir::BitField<31, 0> f{std::array<std::byte, 8>{}};\n", None),
    Case("one bit compiles",
         "Kvasir::BitField<5, 5> f{std::array<std::byte, 8>{}};\n", None),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
