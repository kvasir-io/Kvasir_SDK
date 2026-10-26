#!/usr/bin/env python3
"""Kvasir::Flash::CellStore refuses a store that cannot keep its promise: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output.

    test_cell_store_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> --tests <Kvasir_SDK/tests> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Runtime/CellStore.hpp"
#include "support/FakeNorFlash.hpp"
#include <string>
struct Tag;
template<std::uint32_t Sectors> using F = Kvasir::Test::FakeNorFlash<Tag, 4096, 256, Sectors>;
struct Small { std::uint32_t a; };
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("one sector",
         "auto x = Kvasir::Flash::CellStore<F<1>, Small>::load();\n",
         ["two blocks at least"]),
    Case("a cell of two sectors in three",
         "struct Big { char b[6000]; };\nauto x = Kvasir::Flash::CellStore<F<3>, Big>::load();\n",
         ["whole number of blocks"]),
    Case("a cell of two sectors in two",
         "struct Big { char b[6000]; };\nauto x = Kvasir::Flash::CellStore<F<2>, Big>::load();\n",
         ["two blocks at least"]),
    Case("not trivially copyable",
         "struct S { std::string s; };\nauto x = Kvasir::Flash::CellStore<F<2>, S>::forget;\n",
         ["trivially copyable"]),
    Case("CellBytes not a page multiple",
         "struct C : Kvasir::Flash::CellStoreDefaults { static constexpr std::size_t cellBytes = 4000; };\n"
         "auto x = Kvasir::Flash::CellStore<F<2>, Small, C>::forget;\n",
         ["page multiple"]),
    Case("a config that does not derive from CellStoreDefaults",
         "struct C { static constexpr bool skipUnchanged = false; };\n"
         "auto x = Kvasir::Flash::CellStore<F<2>, Small, C>::forget;\n",
         ["derives from Kvasir::Flash::CellStoreDefaults"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
