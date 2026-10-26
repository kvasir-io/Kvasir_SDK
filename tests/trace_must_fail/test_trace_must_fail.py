#!/usr/bin/env python3
"""Kvasir::Trace::Ring refuses a capacity that is not a power of two, a capacity past 16 bits, a ring without
fields and a record() with the wrong number of values, each with its own message: compile each snippet with
-fsyntax-only and expect the message - and a valid ring must still compile.

    test_trace_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Util/Trace.hpp"\n'

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("capacity 3",
         'using R = Kvasir::Trace::Ring<"r", 3, "a">;\nauto n = R::count();\n',
         ["the capacity of a trace ring is a power of two"]),
    Case("capacity 0x10000",
         'using R = Kvasir::Trace::Ring<"r", 0x10000, "a">;\nauto n = R::count();\n',
         ["the capacity has to fit the header's 16 bits"]),
    Case("no fields",
         'using R = Kvasir::Trace::Ring<"r", 4>;\nauto n = R::count();\n',
         ["a trace ring needs at least one field"]),
    Case("record() arity",
         'using R = Kvasir::Trace::Ring<"r", 4, "a", "b">;\nvoid f() { R::record(1); }\n',
         ["record() takes one value per field of the ring"]),
    Case("a valid ring compiles",
         'using R = Kvasir::Trace::Ring<"r", 0x8000, "a", "b">;\nvoid f() { R::record(1, 2); }\n',
         None),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
