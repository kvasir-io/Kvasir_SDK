#!/usr/bin/env python3
"""Kvasir::Health refuses a recovery it cannot keep within the watchdog: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output. A control snippet must compile.

    test_health_must_fail.py <c++ compiler> <Kvasir_SDK/src>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '''#include "kvasir/Util/Health.hpp"
#include <chrono>
#include <cstdint>
using namespace std::chrono_literals;
struct Clock {
    using rep = std::int64_t; using period = std::milli; using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock>;
    static time_point now() { return {}; }
};
struct Watchdog { static constexpr auto Timeout = 100ms; static void feed() {} };
template<typename C> struct Startup {
    template<typename Hook> using ExtendsOn = brigand::list<Kvasir::Health::Check<C>>;
    template<typename Hook> static void run() {}
};
'''

# (name, snippet, substrings every compiler prints; empty: must compile)
CASES = [
    ("control: one rung, 2 ms budget, 100 ms timeout",
     "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
     "           static void onStarved() {} };\n"
     "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
     []),
    ("recoveryAttempts = 0 with an onStarved",
     "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
     "           static constexpr auto recoveryAttempts = 0; static void onStarved() {} };\n"
     "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
     ["recoveryAttempts of a check with onStarved is 1 .. 15"]),
    ("recoveryAttempts = 16",
     "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
     "           static constexpr auto recoveryAttempts = 16; static void onStarved() {} };\n"
     "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
     ["recoveryAttempts of a check with onStarved is 1 .. 15"]),
    ("budget over the watchdog's timeout",
     "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
     "           static void onStarved() {} };\n"
     "struct Cfg { static constexpr auto period = 10ms; static constexpr auto recoveryBudget = 90ms; };\n"
     "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock, Cfg>::service(); }\n",
     ["recoveryBudget must stay below the watchdog's timeout"]),
    ("onStarved returning something else",
     "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
     "           static int onStarved() { return 0; } };\n"
     "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
     ["onStarved returns void or Kvasir::Health::Recovery"]),
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
            if not want:
                bad = r.returncode != 0
                missing = []
            else:
                missing = [w for w in want if w not in out]
                bad = r.returncode == 0 or bool(missing)
            if bad:
                failed += 1
                print(
                    f"FAIL {name}: exit {r.returncode}, missing {missing}\n{out[:3000]}")
            else:
                print(f"ok   {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
