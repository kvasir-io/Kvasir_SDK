#!/usr/bin/env python3
"""KVASIR_CONFIG_OR (kvasir/Util/Config.hpp) refuses what they must: compile each snippet with
-fsyntax-only, expect it to fail, and expect the reason in the compiler's output. A control case must compile.

    test_config_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Util/Config.hpp"
#include <cstdint>
template<typename Config> struct Knobs {
    static constexpr bool stats = KVASIR_CONFIG_OR(Config, stats, true);
    static constexpr std::uint8_t narrow = std::uint8_t{KVASIR_CONFIG_OR(Config, narrow, std::uint8_t{1})};
};
struct Defaults { static constexpr bool stats = false; };
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("control: a config derived from the defaults compiles",
         "struct C : Defaults {};\nconstexpr bool x = Knobs<C>::stats;\n", None),
    Case("outside a template",
         "struct A {};\nconstexpr bool x = KVASIR_CONFIG_OR(A, stats, true);\n", ["stats"]),
    Case("a braced narrowing",
         "struct C { static constexpr int narrow = 300; };\nconstexpr auto x = Knobs<C>::narrow;\n", ["narrow"]),
    Case("a member that is not a constant expression (consteval)",
         "struct C { static inline bool stats = true; };\nconstexpr bool x = Knobs<C>::stats;\n",
         ["constant expression"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
