#!/usr/bin/env python3
"""Start<S>'s safe-state check (kvasir/StartUp/SafeState.hpp): a Startup list that extends Hook::SafeState while the
image's Panic::FatalStartReaction is still NoFatalStart must not compile, with the check's message; the wired list and a list
without SafeState entries compile. A FatalStart specialisation placed after the check read the hook is the compiler's
"explicit specialization after instantiation" error, not a silent miss.

    test_safe_state_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = """#include "kvasir/StartUp/SafeState.hpp"
#include "kvasir/Port/Hooks.hpp"
#include "kvasir/Common/Tags.hpp"
inline void off() {}
template<typename Hook> using L = Kvasir::MPL::Filter<Kvasir::Startup::Detail::OfHook<Hook>,
    brigand::list<Kvasir::Startup::Extend<Kvasir::Hook::%s, &off>>>;
struct Startup { template<typename Hook> using ExtendsOn = L<Hook>; };
// what Start<S> asserts
template<typename S> struct Start {
    static_assert(Kvasir::Startup::Detail::safeStateWired<S,
        typename Kvasir::Port::LateT<Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User>, S>::type>());
};
"""

WIRE = """template<> struct Kvasir::Panic::FatalStartReaction<Kvasir::Tag::User> {
    using type = Kvasir::Startup::SafeStateOf<::Startup>; };
"""

INST = "template struct Start<Startup>;\nint main() { return 0; }\n"

# (name, the hook the entry extends, the tail, the wanted message; None: must compile)
ROWS = [
    ("the wiring with a plain `Startup`: inside Kvasir::Panic that names the namespace Kvasir::Startup", "SafeState",
     WIRE.replace("<::Startup>", "<Startup>") + INST, "Startup"),
    ("control: SafeState entries, wired (want None)", "SafeState", WIRE + INST, None),
    ("control: no SafeState entry, nothing wired (want None)", "MainLoop", INST, None),
    ("SafeState entries, no fatal start", "SafeState", INST,
     "the Startup list extends Kvasir::Hook::SafeState, but the fatal start does not run it"),
    ("the wiring after the check", "SafeState", "template struct Start<Startup>;\n" + WIRE + "int main() { return 0; }\n",
     "after instantiation"),
]


CASES = [Case(name, tail, None if want is None else [want], head=HEAD % hook)
         for name, hook, tail, want in ROWS]

if __name__ == "__main__":
    sys.exit(run("", CASES))
