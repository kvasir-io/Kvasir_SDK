#!/usr/bin/env python3
"""Kvasir::Crc::Engine refuses what it cannot compute right: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output.

    test_crc_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Util/Crc.hpp"\nnamespace C = Kvasir::Crc;\n'

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("wrong check value",
         """constexpr C::Params<std::uint8_t> bad{8, 0x07, 0x00, false, false, 0x00, 0xF5};
auto x = C::Engine<bad, 0>::compute(std::string_view{"abc"});
""",
         ["the parameters do not give their own check value"]),
    Case("table size",
         """auto x = C::Engine<C::Presets::crc32IsoHdlc, 8>::compute(std::string_view{"abc"});
""",
         ["TableSize"]),
    Case("resume with refin != refout",
         """constexpr C::Params<std::uint16_t> umts{12, 0x80F, 0x000, false, true, 0x000, 0xDAF};
auto x = C::Engine<umts, 0>::resume(0);
""",
         ["refin == P.refout"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
