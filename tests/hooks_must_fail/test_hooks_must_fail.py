#!/usr/bin/env python3
"""The main-loop hook refuses what would run twice a turn, and Health::Service<> refuses a missing supervisor binding:
compile each snippet with -fsyntax-only, expect it to fail, and expect the reason in the compiler's output. A control
snippet must compile.

    test_hooks_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Port/Extends.hpp"
struct Bus { static void turn() {} using Extends = Kvasir::Startup::Extend<Kvasir::Hook::MainLoop, &Bus::turn>; };
struct Led { static void turn() {} };
template<typename... Ts> void loop() {
    (void)Kvasir::Startup::Detail::dispatch<Kvasir::Hook::MainLoop, Kvasir::Turn>(static_cast<brigand::list<Ts...>*>(nullptr));
    Kvasir::Startup::Detail::dispatch<Kvasir::Hook::MainLoop, void>(static_cast<brigand::list<Ts...>*>(nullptr));
}
'''

HEALTH_HEAD = '''#include "kvasir/Health/Health.hpp"
#include <chrono>
#include <cstdint>
using namespace std::chrono_literals;
struct Clock {
    using rep = std::int64_t; using period = std::milli; using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock>;
    static time_point now() { return {}; }
};
struct Watchdog { static constexpr auto timeout = 100ms; static void feed() {} };
struct C { static constexpr std::string_view name = "c"; static constexpr auto maxInterval = 50ms; };
struct Startup {
    template<typename Hook> using ExtendsOn = brigand::list<Kvasir::Health::Check<C>>;
    template<typename Hook> static void run() {}
};
using Health = Kvasir::Health::Supervisor<Startup, Watchdog, Clock>;
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("control: a hand-driven component without an Extends",
         "void f() { loop<Bus, Kvasir::Startup::DrivenByHand<Led>>(); }\n",
         []),
    Case("DrivenByHand<Bus> while Bus extends the hook",
         "void f() { loop<Bus, Kvasir::Startup::DrivenByHand<Led, Bus>>(); }\n",
         ["also driven by", "twice a turn"]),
    Case("control: Health::Service<> with its binding",
         "template<> struct Kvasir::Health::SupervisorFor<Kvasir::Tag::User> { using type = ::Health; };\n"
         "void f() { (void)Kvasir::Startup::Detail::dispatch<Kvasir::Hook::MainLoop, Kvasir::Turn>(\n"
         "    static_cast<brigand::list<Kvasir::Health::Service<>>*>(nullptr)); }\n",
         [], head=HEALTH_HEAD),
    Case("Health::Service<> without the SupervisorFor binding",
         "void f() { (void)Kvasir::Startup::Detail::dispatch<Kvasir::Hook::MainLoop, Kvasir::Turn>(\n"
         "    static_cast<brigand::list<Kvasir::Health::Service<>>*>(nullptr)); }\n",
         ["Health::Service<> without"], head=HEALTH_HEAD),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
