#!/usr/bin/env python3
"""Kvasir::literals refuses what it cannot represent: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output.

    test_literals_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...

A suffix's own range is validateValue's "integer literal overflow"; a literal past the 64-bit parse
type and a character that is no digit are named by the function the consteval parser reaches
(Util/detail/constant_parser.hpp). A digit too big for its base (09_u32) is refused by the lexer
before the parser sees it, so it is not a case here.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Util/literals.hpp"\nusing namespace Kvasir::literals;\n'

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("256_u8", "auto x = 256_u8;\n", ["integer literal overflow"]),
    Case("4294967296_u32", "auto x = 4294967296_u32;\n",
         ["integer literal overflow"]),
    Case("2147483648_i", "auto x = 2147483648_i;\n",
         ["integer literal overflow"]),
    Case("4294967296_u", "auto x = 4294967296_u;\n",
         ["integer literal overflow"]),
    Case("18446744073709551616_u64", "auto x = 18446744073709551616_u64;\n",
         ["integerLiteralOverflowsItsParseType"]),
    Case("0x1'0000'0000'0000'0000_u64", "auto x = 0x1'0000'0000'0000'0000_u64;\n",
         ["integerLiteralOverflowsItsParseType"]),
    Case("9223372036854775808_i64", "auto x = 9223372036854775808_i64;\n",
         ["integerLiteralOverflowsItsParseType"]),
    Case("1.5_u32", "auto x = 1.5_u32;\n", ["literalCharacterIsNotADigit"]),
]

# the control: the same head with a valid literal compiles, so a failure above is the literal's
CONTROL = "auto x = 0xffff'ffff'ffff'ffff_u64;\n"


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES, flags=[
             "-fno-exceptions"], controls_first=[Case("control", CONTROL)]))
