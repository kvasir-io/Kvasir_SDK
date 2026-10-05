#!/usr/bin/env python3
"""Kvasir::Flash::CellStore refuses a store that cannot keep its promise: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output.

    test_cell_store_must_fail.py <c++ compiler> <Kvasir_SDK/src> <Kvasir_SDK/tests>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '''#include "kvasir/Util/CellStore.hpp"
#include "support/FakeNorFlash.hpp"
#include <string>
struct Tag;
template<std::uint32_t Sectors> using F = Kvasir::Test::FakeNorFlash<Tag, 4096, 256, Sectors>;
struct Small { std::uint32_t a; };
'''

# (name, snippet, substrings every compiler prints)
CASES = [
    ("one sector",
     "auto x = Kvasir::Flash::CellStore<F<1>, Small>::load();\n",
     ["two blocks at least"]),
    ("a cell of two sectors in three",
     "struct Big { char b[6000]; };\nauto x = Kvasir::Flash::CellStore<F<3>, Big>::load();\n",
     ["whole number of blocks"]),
    ("a cell of two sectors in two",
     "struct Big { char b[6000]; };\nauto x = Kvasir::Flash::CellStore<F<2>, Big>::load();\n",
     ["two blocks at least"]),
    ("not trivially copyable",
     "struct S { std::string s; };\nauto x = Kvasir::Flash::CellStore<F<2>, S>::forget;\n",
     ["trivially copyable"]),
    ("CellBytes not a page multiple",
     "struct C : Kvasir::Flash::CellStoreDefaults { static constexpr std::size_t CellBytes = 4000; };\n"
     "auto x = Kvasir::Flash::CellStore<F<2>, Small, C>::forget;\n",
     ["page multiple"]),
]


def main() -> int:
    cxx, src, tests = sys.argv[1], sys.argv[2], sys.argv[3]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, body, want in CASES:
            f = Path(tmp) / "snippet.cpp"
            f.write_text(HEAD + body)
            r = subprocess.run([cxx, "-std=c++26", "-fsyntax-only", f"-I{src}", f"-I{tests}", str(f)],
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
