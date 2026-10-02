#!/usr/bin/env python3
"""Kvasir::Prescaler's failures are compile errors that carry the numbers: compile each snippet with
-fsyntax-only, expect it to fail, and expect the numbers in the compiler's output.

    test_prescaler_must_fail.py <c++ compiler> <Kvasir_SDK/src>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '#include "kvasir/Util/Prescaler.hpp"\nnamespace P = Kvasir::Prescaler;\n'

# (name, snippet, substrings every compiler prints, substrings only clang prints)
CASES = [
    ("tolerance message",
     """constexpr auto u = P::fromMultiplier(48'000'000, 3'500'000,
                                     {.first = 1, .last = 65536, .den = 1u << 20});
template<int> struct S {
    static constexpr bool Ok = [] {
        P::assertInTolerance<u.achieved, 3'500'000, P::Tolerance{std::ratio<5, 1000>{}},
                             "USART baud rate">();
        return true;
    }();
    static_assert(Ok);
};
S<0> s;
""",
     ["USART baud rate: wanted 3500000 Hz, got 3000000.0 Hz (-142857 ppm), allowed 5000 ppm"], []),
    ("per-device tolerance",
     """consteval int timing(std::uint32_t hz) {
    P::requireInTolerance(P::Rational{48'000'000, 112}, hz, P::Tolerance{std::ratio<1, 100>{}});
    return 1;
}
constexpr int t = timing(400'000);
""",
     ["rateOutOfTolerance", "timing(400000)"],
     ["rateOutOfTolerance(400000, 428571429, 71429, 10000)"]),
    ("out of reach",
     """consteval int setup(std::uint32_t hz) {
    P::rateOutOfReach(hz, 93'750, 24'000'000'000);
    return 1;
}
constexpr int t = setup(10);
""",
     ["rateOutOfReach", "setup(10)"],
     ["rateOutOfReach(10, 93750, 24000000000)"]),
    ("overflow",
     """constexpr auto r = P::fromRange(~std::uint64_t{0}, 7, {.first = 1, .last = 256});
""",
     ["prescalerArithmeticOverflowsSixtyFourBits"], []),
]


def main() -> int:
    cxx, src = sys.argv[1], sys.argv[2]
    version = subprocess.run(
        [cxx, "--version"], capture_output=True, text=True).stdout
    is_clang = "clang" in version
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, body, everywhere, clang_only in CASES:
            f = Path(tmp) / "snippet.cpp"
            f.write_text(HEAD + body)
            r = subprocess.run([cxx, "-std=c++26", "-fsyntax-only", f"-I{src}", str(f)],
                               capture_output=True, text=True)
            out = r.stdout + r.stderr
            want = everywhere + (clang_only if is_clang else [])
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
