#!/usr/bin/env python3
"""What is read or stored as an object's bytes says what it needs of the type: compile each snippet with
-fsyntax-only, expect it to fail, and expect the reason in the compiler's output.

A Persistent's CRC runs over its value's bytes, so a value with padding is refused (Util/PaddingFree.hpp);
Memory::loadPlain is one unsigned word, a record is loadBytes + std::bit_cast.

    test_object_bytes_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Register/Memory.hpp"
#include "kvasir/Util/Persistent.hpp"
#include <cstdint>
struct Tag { static constexpr std::uint32_t id = Kvasir::persistentId("KTST"); };
struct Words { std::uint32_t a; std::uint32_t b; };
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("a Persistent value of whole words",
         "Kvasir::Persistent<Words, Tag> p;\n",
         None),
    Case("a Persistent value with padding",
         "struct Gap { std::uint8_t a; std::uint32_t b; };\nKvasir::Persistent<Gap, Tag> p;\n",
         ["T has no padding"]),
    Case("a Persistent value of floats has none",
         "struct Gains { float p; float i; };\nKvasir::Persistent<Gains, Tag> p;\n",
         None),
    Case("a plain load of a word",
         "auto x = Kvasir::Memory::loadPlain<std::uint32_t>(0x1000);\n",
         None),
    Case("a plain load of a struct",
         "auto x = Kvasir::Memory::loadPlain<Words>(0x1000);\n",
         ["a plain load is one unsigned word"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
