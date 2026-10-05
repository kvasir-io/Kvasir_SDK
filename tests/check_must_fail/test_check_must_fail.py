#!/usr/bin/env python3
"""Kvasir::Check in a constant evaluation: a failing check stops the compile and names itself.

    test_check_must_fail.py <c++ compiler> <Kvasir_SDK/src>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '#include "kvasir/Util/Check.hpp"\n'

# (name, snippet, substrings every compiler prints)
CASES = [
    ("binary check in a constant evaluation",
     """constexpr int g(int v) { KVASIR_CHECK_LE(v, 10); return v; }
static_assert(g(30) == 30);
""",
     ["checkFailedInConstantExpression"]),
    ("unary check in a constant evaluation",
     """constexpr int g(int v) { KVASIR_CHECK(v < 10, "v {}", v); return v; }
constexpr int x = g(30);
""",
     ["checkFailedInConstantExpression"]),
]


def main() -> int:
    cxx, src = sys.argv[1], sys.argv[2]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, body, want in CASES:
            f = Path(tmp) / "snippet.cpp"
            f.write_text(HEAD + body)
            r = subprocess.run([cxx, "-std=c++26", "-fsyntax-only", f"-I{src}", str(f)],
                               capture_output=True, text=True)
            out = r.stdout + r.stderr
            missing = [w for w in want if w not in out]
            if r.returncode == 0 or missing:
                failed += 1
                print(
                    f"FAIL {name}: exit {r.returncode}, missing {missing}\n{out[:3000]}")
            else:
                print(f"ok   {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
