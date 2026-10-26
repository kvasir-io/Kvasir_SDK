#!/usr/bin/env python3
"""Kvasir::BootGuard::OnLimit::Halt<Startup, Stop> refuses a Stop that cannot stop the watchdog: compile each snippet with
-fsyntax-only, expect it to fail, and expect the reason in the compiler's output. The control snippets must compile.

    test_boot_guard_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Runtime/BootGuardRules.hpp"
#include "kvasir/Health/Health.hpp"
#include <chrono>
#include <cstdint>
#include <string_view>
using namespace std::chrono_literals;
struct Clock {
    using rep = std::int64_t; using period = std::milli; using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock>;
    static time_point now() { return {}; }
};
struct C { static constexpr std::string_view name = "c"; static constexpr auto maxInterval = 50ms; };
struct Startup {
    template<typename Hook> using ExtendsOn = brigand::list<Kvasir::Health::Check<C>>;
    template<typename Hook> static void run() {}
};
// gated (chip_rp_common's watchdog with Config::gatedFeed): every call takes the supervisor's key
struct Gated { static constexpr auto timeout = 100ms; static void feed(Kvasir::Health::FeedKey) {}
               static void disarm(Kvasir::Health::FeedKey) {} };
// one that cannot be stopped at all (an STM32 IWDG)
struct Unstoppable { static constexpr auto timeout = 100ms; static void feed() {} };
struct Plain { static constexpr auto timeout = 100ms; static void feed() {} static void disarm() {} };
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("control: Halt<Startup, Supervisor over a gated watchdog>",
         "void f() { Kvasir::BootGuard::OnLimit::Halt<Startup, Kvasir::Health::Supervisor<Startup, Gated, Clock>>::stopWatchdog(); }\n",
         []),
    Case("control: Halt<Startup, an ungated watchdog> and Halt<Startup>",
         "void f() { Kvasir::BootGuard::OnLimit::Halt<Startup, Plain>::stopWatchdog(); Kvasir::BootGuard::OnLimit::Halt<Startup>::stopWatchdog(); }\n",
         []),
    Case("a gated watchdog passed directly",
         "void f() { Kvasir::BootGuard::OnLimit::Halt<Startup, Gated>::stopWatchdog(); }\n",
         ["takes the health supervisor's FeedKey"]),
    Case("a supervisor whose watchdog cannot be stopped",
         "void f() { Kvasir::BootGuard::OnLimit::Halt<Startup, Kvasir::Health::Supervisor<Startup, Unstoppable, Clock>>::stopWatchdog(); }\n",
         ["cannot be stopped"]),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
