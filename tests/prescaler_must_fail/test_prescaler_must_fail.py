#!/usr/bin/env python3
"""Kvasir::Prescaler's failures are compile errors that carry the numbers: compile each snippet with
-fsyntax-only, expect it to fail, and expect the numbers in the compiler's output.

    test_prescaler_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '#include "kvasir/Util/Prescaler.hpp"\nnamespace P = Kvasir::Prescaler;\n'

# Case(name, snippet, substrings every compiler prints, substrings only clang prints)
CASES = [
    Case("tolerance message",
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
    Case("per-device tolerance",
         """consteval int timing(std::uint32_t hz) {
    P::requireInTolerance(P::Rational{48'000'000, 112}, hz, P::Tolerance{std::ratio<1, 100>{}});
    return 1;
}
constexpr int t = timing(400'000);
""",
         ["rateOutOfTolerance", "timing(400000)"],
         ["rateOutOfTolerance(400000, 428571429, 71429, 10000)"]),
    Case("out of reach",
         """consteval int setup(std::uint32_t hz) {
    P::rateOutOfReach(hz, 93'750, 24'000'000'000);
    return 1;
}
constexpr int t = setup(10);
""",
         ["rateOutOfReach", "setup(10)"],
         ["rateOutOfReach(10, 93750, 24000000000)"]),
    Case("overflow",
         """constexpr auto r = P::fromRange(~std::uint64_t{0}, 7, {.first = 1, .last = 256});
""",
         ["prescalerArithmeticOverflowsSixtyFourBits"], []),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
