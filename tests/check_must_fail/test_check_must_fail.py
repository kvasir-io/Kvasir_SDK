#!/usr/bin/env python3
"""Kvasir::Check in a constant evaluation: a failing check stops the compile and names itself.

    test_check_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Runtime/Check.hpp"\n'

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("binary check in a constant evaluation",
         """constexpr int g(int v) { KVASIR_CHECK_LE(v, 10); return v; }
static_assert(g(30) == 30);
""",
         ["checkFailedInConstantExpression"]),
    Case("unary check in a constant evaluation",
         """constexpr int g(int v) { KVASIR_CHECK(v < 10, "v {}", v); return v; }
constexpr int x = g(30);
""",
         ["checkFailedInConstantExpression"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
