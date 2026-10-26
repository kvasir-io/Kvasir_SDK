#!/usr/bin/env python3
"""The port contract's named errors (kvasir/Port/Check.hpp): a fake chip and core with one piece left out each, then
Port::checkCore0<>(). Each case must fail with its own message; the control (every piece there) must compile.

    test_port_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

PIECES = {
    "first": """namespace Kvasir::Startup {
template<typename... Ts> struct FirstInitStep<Tag::User, Ts...> { void operator()() const {} };
}
""",
    "traits": """namespace Kvasir::Nvic {
template<> struct InterruptOffsetTraits<Kvasir::Tag::User> { static constexpr int begin = -14; static constexpr int end = 32; };
}
""",
    "clean": "namespace Kvasir::Startup::Core { inline void cleanCore() {} }\n",
    "nvic": "namespace Kvasir::Nvic { inline void clearAllEnablesAndPending() {} }\n",
}

HEAD = """#include "kvasir/Common/Tags.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Port/Interrupts.hpp"
"""

TAIL = """#include "kvasir/Port/Check.hpp"
struct S {};
static_assert(Kvasir::Port::checkCore0<S, decltype(Kvasir::Startup::Core::cleanCore()),
                                       decltype(Kvasir::Nvic::clearAllEnablesAndPending())>());
int main() { return 0; }
"""

# (name, pieces left out, wanted message)
ROWS = [
    ("control: every piece there (want None)", [], None),
    ("a chip without FirstInitStep", [
     "first"], "provides no Startup::FirstInitStep<Tag::User> (chip/StartUp.hpp)"),
    ("a chip without InterruptOffsetTraits", ["traits"],
     "provides no Nvic::InterruptOffsetTraits<Tag::User> with begin/end"),
    ("a core without cleanCore", ["clean"],
     "the core package provides no Startup::Core::cleanCore() (core/StartUp.hpp)"),
    ("a core without the public NVIC clear", ["nvic"],
     "the core package provides no Nvic::clearAllEnablesAndPending()"),
]


CASES = [Case(name, "".join(v for k, v in PIECES.items() if k not in leave_out) + TAIL,
              None if want is None else [want]) for name, leave_out, want in ROWS]

if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
