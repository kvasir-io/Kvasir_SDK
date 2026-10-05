#!/usr/bin/env python3
"""Kvasir::Crc::Engine refuses what it cannot compute right: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output.

    test_crc_must_fail.py <c++ compiler> <Kvasir_SDK/src>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '#include "kvasir/Util/Crc.hpp"\nnamespace C = Kvasir::Crc;\n'

# (name, snippet, substrings every compiler prints)
CASES = [
    ("wrong check value",
     """constexpr C::Params<std::uint8_t> bad{8, 0x07, 0x00, false, false, 0x00, 0xF5};
auto x = C::Engine<bad, 0>::compute(std::string_view{"abc"});
""",
     ["the parameters do not give their own check value"]),
    ("table size",
     """auto x = C::Engine<C::Presets::crc32IsoHdlc, 8>::compute(std::string_view{"abc"});
""",
     ["TableSize"]),
    ("resume with refin != refout",
     """constexpr C::Params<std::uint16_t> umts{12, 0x80F, 0x000, false, true, 0x000, 0xDAF};
auto x = C::Engine<umts, 0>::resume(0);
""",
     ["refin == P.refout"]),
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
