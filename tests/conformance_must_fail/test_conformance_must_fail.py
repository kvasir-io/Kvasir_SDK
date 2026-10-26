#!/usr/bin/env python3
"""The contract rows of the conformance trees fail in both directions with the row's message: a REQUIRE of a row the
fake chip lacks, and a GAP of a row the fake chip has. Each snippet is compiled with -fsyntax-only, must fail, and must
print the expected text.

    test_conformance_must_fail.py --cxx <c++ compiler> --src <Kvasir_SDK/src> --tests <Kvasir_SDK/tests> [--flag=<flag>]...
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tooling"))
from must_fail import Case, run  # noqa: E402

CHIP = '''#include "kvasir/Port/ResetKind.hpp"
namespace Kvasir::PM { inline int resetCause() { return 0; } }
'''
CONTRACT = '#include "conformance/ChipContract.hpp"\n'

# Case(name, snippet, substrings every compiler prints; None: must compile)
CASES = [
    Case("REQUIRE of a missing function",
         CHIP + CONTRACT +
         'KVASIR_CONFORMANCE_REQUIRE("PM::resetKind()", Kvasir::Conformance::Row::resetKind);\n',
         ["chip contract: PM::resetKind() missing"]),
    Case("REQUIRE of a missing constant",
         CHIP + CONTRACT +
         'KVASIR_CONFORMANCE_REQUIRE("Chip::Part", Kvasir::Conformance::Row::chipPart);\n',
         ["chip contract: Chip::Part missing"]),
    Case("REQUIRE of a hook the chip did not specialise",
         CHIP + CONTRACT +
         'KVASIR_CONFORMANCE_REQUIRE("Register::CpuHzCeiling<Tag::User>", Kvasir::Conformance::Row::cpuHzCeiling);\n',
         ["chip contract: Register::CpuHzCeiling<Tag::User> missing"]),
    Case("GAP of a present function",
         CHIP +
         'namespace Kvasir::Chip { inline void reboot() {} }\n' + CONTRACT
         + 'KVASIR_CONFORMANCE_GAP("Chip::reboot()", Kvasir::Conformance::Row::chipReboot);\n',
         ["chip contract: Chip::reboot() exists now - move it to the required list"]),
    Case("GAP of a present core function",
         'namespace Kvasir::Core { inline bool haltingDebug() { return false; } }\n'
         + '#include "conformance/CoreContract.hpp"\n'
         + 'KVASIR_CORE_CONFORMANCE_GAP("Core::haltingDebug()", Kvasir::Conformance::Row::coreHaltingDebug);\n',
         ["core contract: Core::haltingDebug() exists now - move it to the required list"]),
    Case("GAP of a present type",
         'namespace Kvasir::Startup::Core { struct SecondaryCoreTraits {}; }\n'
         + '#include "conformance/CoreContract.hpp"\n'
         + 'KVASIR_CORE_CONFORMANCE_GAP("Startup::Core::SecondaryCoreTraits",'
         + ' Kvasir::Conformance::Row::coreSecondaryCoreTraits);\n',
         ["core contract: Startup::Core::SecondaryCoreTraits exists now - move it to the required list"]),
    Case("REQUIRE of a missing core function",
         '#include "conformance/CoreContract.hpp"\n'
         + 'KVASIR_CORE_CONFORMANCE_REQUIRE("Core::Debug::isDebuggerConnected()",'
         + ' Kvasir::Conformance::Row::coreDebugIsDebuggerConnected);\n',
         ["core contract: Core::Debug::isDebuggerConnected() missing"]),
    Case("a resetKind() of the wrong type is missing",
         CHIP +
         'namespace Kvasir::PM { inline int resetKind() { return 0; } }\n' +
         CONTRACT
         + 'KVASIR_CONFORMANCE_REQUIRE("PM::resetKind()", Kvasir::Conformance::Row::resetKind);\n',
         ["chip contract: PM::resetKind() missing"]),
]


if __name__ == "__main__":
    sys.exit(run("", CASES))
