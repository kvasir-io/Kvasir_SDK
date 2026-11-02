#!/usr/bin/env python3
"""Kvasir::Test (src/kvasir/Test): what a test list, an application's self-test and a watchdog test refuse.

    test_device_test_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

# the log macros as no-ops (Test.hpp then includes no uc_log), string_constant from the SDK's submodule
HEAD = """#define UC_LOG_T(...) ((void)0)
#define UC_LOG_D(...) ((void)0)
#define UC_LOG_I(...) ((void)0)
#define UC_LOG_W(...) ((void)0)
#define UC_LOG_E(...) ((void)0)
#define UC_LOG_C(...) ((void)0)
#include "kvasir/Test/OnRequest.hpp"
#include <chrono>
struct FakeClock {
    using rep = long long; using period = std::micro; using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<FakeClock>; static constexpr bool is_steady = true;
    static time_point now() { return time_point{}; }
};
struct Cfg : Kvasir::Test::DefaultConfig { using Clock = FakeClock; };
KVASIR_TEST(S, Plain) {}
KVASIR_TEST_WITH(S, Panics, {.outcome = Kvasir::Test::Outcome::panic}) {}
KVASIR_TEST_WITH(S, Bites, {.outcome = Kvasir::Test::Outcome::watchdog}) {}
"""

FLAGS = ["-I@SRC@/../uc_log/remote_fmt/string_constant/src"]

CASES = [
    Case("control: a self-test of returning tests compiles",
         "using S = Kvasir::Test::OnRequest<Cfg, Kvasir::Test::List<KvasirTests::S::Plain>>;\n"
         "void f() { S::runtimeInit(); (void)S::turn(); }\n",
         None, flags=FLAGS),
    Case("a list element that is no test",
         "using F = Kvasir::Test::Detail::Flatten<Kvasir::Test::List<int>>::type;\nF f;\n",
         ["neither a test"], flags=FLAGS),
    Case("an application's self-test with a test that must panic",
         "using S = Kvasir::Test::OnRequest<Cfg, Kvasir::Test::List<KvasirTests::S::Panics>>;\n"
         "void f() { (void)S::turn(); }\n",
         ["every test of an application's self-test returns"], flags=FLAGS),
    Case("a watchdog test without Config::Watchdog",
         "using E = Kvasir::Test::Detail::Engine<Cfg, Kvasir::Test::List<KvasirTests::S::Bites>>;\n"
         "void f(Kvasir::Test::RunState& s) { E::runTable[0](s); }\n",
         ["needs Config::Watchdog"], flags=FLAGS),
]


if __name__ == "__main__":
    sys.exit(run(HEAD, CASES))
