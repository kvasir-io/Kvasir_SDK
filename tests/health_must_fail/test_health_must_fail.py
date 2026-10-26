#!/usr/bin/env python3
"""Kvasir::Health refuses a recovery it cannot keep within the watchdog: compile each snippet with -fsyntax-only,
expect it to fail, and expect the reason in the compiler's output. A control snippet must compile. The lean
Health/Check.hpp cases prove that a driver which declares a check gets neither the policies nor the supervisor.

    test_health_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

HEAD = '''#include "kvasir/Health/Health.hpp"
#include <chrono>
#include <cstdint>
using namespace std::chrono_literals;
struct Clock {
    using rep = std::int64_t; using period = std::milli; using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<Clock>;
    static time_point now() { return {}; }
};
struct Watchdog { static constexpr auto timeout = 100ms; static void feed() {} };
template<typename C> struct Startup {
    template<typename Hook> using ExtendsOn = brigand::list<Kvasir::Health::Check<C>>;
    template<typename Hook> static void run() {}
};
'''

# a driver's view: only the check vocabulary
CHECK_HEAD = '''#include "kvasir/Health/Check.hpp"
#include <chrono>
using namespace std::chrono_literals;
struct C { static constexpr std::string_view name = "c"; static constexpr auto maxInterval = 50ms; };
'''

# the seam (Health/LinkCheck.hpp): what a part must have
LINK_HEAD = '''#include "kvasir/Health/LinkCheck.hpp"
#include <chrono>
using namespace std::chrono_literals;
struct C { static constexpr std::string_view name = "c"; static constexpr auto maxInterval = 50ms; };
struct Part { static Kvasir::Link link() { return Kvasir::Link::answering; } };
struct NoLink { static bool failed() { return false; } };
struct BoolLink { static bool link() { return true; } };
'''

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("control: one rung, 2 ms budget, 100 ms timeout",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
         "           static void onStarved() {} };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
         []),
    Case("recoveryAttempts = 0 with an onStarved",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
         "           static constexpr auto recoveryAttempts = 0; static void onStarved() {} };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
         ["recoveryAttempts of a check with onStarved is 1 .. 15"]),
    Case("recoveryAttempts = 16",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
         "           static constexpr auto recoveryAttempts = 16; static void onStarved() {} };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
         ["recoveryAttempts of a check with onStarved is 1 .. 15"]),
    Case("budget over the watchdog's timeout",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
         "           static void onStarved() {} };\n"
         "struct Cfg { static constexpr auto period = 10ms; static constexpr auto recoveryBudget = 90ms; };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock, Cfg>::service(); }\n",
         ["recoveryBudget must stay below the watchdog's timeout"]),
    Case("onStarved returning something else",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms;\n"
         "           static int onStarved() { return 0; } };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, Watchdog, Clock>::service(); }\n",
         ["onStarved returns void or Kvasir::Health::Recovery"]),
    Case("a watchdog without Timeout",
         "struct C { static constexpr std::string_view name = \"c\"; static constexpr auto maxInterval = 50ms; };\n"
         "struct NoTimeout { static void feed() {} };\n"
         "void f() { Kvasir::Health::Supervisor<Startup<C>, NoTimeout, Clock>::service(); }\n",
         ["the watchdog needs feed() (or feed(FeedKey) when gated) and a Timeout duration"]),
    Case("control: Health/Check.hpp alone declares a check and checks in",
         "using List = brigand::list<Kvasir::Health::Check<C>>;\n"
         "void f() { Kvasir::Health::checkIn<C>(); }\n",
         [], head=CHECK_HEAD),
    Case("Health/Check.hpp alone has no policies",
         "void f() { Kvasir::Health::OnStarvation::StopFeeding p; (void)p; }\n",
         ["OnStarvation"], head=CHECK_HEAD),
    Case("Health/Check.hpp alone has no AnnouncedReset",
         "void f() { (void)Kvasir::AnnouncedReset::announce(1); }\n",
         ["AnnouncedReset"], head=CHECK_HEAD),
    Case("control: LinkCheck on a part with link()",
         "void f() { Kvasir::Health::LinkCheck<Part, C>::turn(); }\n",
         [], head=LINK_HEAD),
    Case("LinkCheck on a part without link()",
         "void f() { Kvasir::Health::LinkCheck<NoLink, C>::turn(); }\n",
         ["Part needs `static Kvasir::Link link()`"], head=LINK_HEAD),
    Case("LinkCheck on a link() that is not a Kvasir::Link",
         "void f() { Kvasir::Health::LinkCheck<BoolLink, C>::turn(); }\n",
         ["Part needs `static Kvasir::Link link()`"], head=LINK_HEAD),
    Case("LinkCheck does not bring the supervisor",
         "void f() { Kvasir::Health::OnStarvation::StopFeeding p; (void)p; }\n",
         ["OnStarvation"], head=LINK_HEAD),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
