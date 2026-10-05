#!/usr/bin/env python3
"""Kvasir's named compile errors: compile each snippet with -fsyntax-only, expect it to fail, and expect the register,
field and resource names in the compiler's output. With KVASIR_NAMED_DIAGNOSTICS=0 the old sentence comes back word
for word.

    test_diagnostics_must_fail.py <c++ compiler> <Kvasir_SDK/src> <Kvasir_SDK/tests>
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HEAD = '''#include "test_registers.hpp"
#include "kvasir/StartUp/Resources.hpp"
#include <array>
#include <string_view>
namespace R = Kvasir::Register;
// a key register that names itself, as svd_converter generates it (RmwHazard's third argument)
struct TESTREG {
    using Addr = R::Address<0x40, 0, 0, std::uint32_t, R::RmwHazard<0xFFFF0000U, false, TESTREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(31, 16), R::ReadWriteAccess, std::uint32_t> key{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 0), R::ReadWriteAccess, std::uint32_t> data{};
    static constexpr std::string_view fmt_string{"test::testreg(\\n\\"key\\": 0x{:x}\\n\\"data\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"key", "data"});
    static constexpr auto field_masks = std::to_array<unsigned>({key.Mask, data.Mask});
};
// a register whose read pops a FIFO
struct POPREG {
    using Addr = R::Address<0x44, 0, 0, std::uint32_t, R::RmwHazard<0, true, POPREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 0), R::ReadWriteAccess, std::uint32_t> data{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(8, 8), R::ReadWriteAccess, std::uint32_t> last{};
    static constexpr std::string_view fmt_string{"test::popreg(\\n\\"data\\": 0x{:x}\\n\\"last\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"data", "last"});
    static constexpr auto field_masks = std::to_array<unsigned>({data.Mask, last.Mask});
};
// a normal register named by NAMED_REGISTERS (phase 2)
struct CTRLREG {
    using Addr = R::Address<0x48, 0, 0, std::uint32_t, R::Named<CTRLREG>>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 4), R::ReadWriteAccess, std::uint32_t> div{};
    static constexpr R::FieldLocation<Addr, R::maskFromRange(0, 0), R::ReadWriteAccess, std::uint32_t> en{};
    static constexpr std::string_view fmt_string{"test::ctrlreg(\\n\\"div\\": 0x{:x}\\n\\"en\\": 0x{:x})"};
    static constexpr auto field_names = std::to_array<std::string_view>({"div", "en"});
    static constexpr auto field_masks = std::to_array<unsigned>({div.Mask, en.Mask});
};
// and one without a name
struct PLAINREG {
    using Addr = R::Address<0x4C, 0, 0, std::uint32_t>;
    static constexpr R::FieldLocation<Addr, R::maskFromRange(7, 4), R::ReadWriteAccess, std::uint32_t> div{};
};
'''

# (name, snippet, extra flags, substrings every compiler prints)
CASES = [
    ("key register, partial write",
     "void f() { apply(write(TESTREG::data, R::value<std::uint32_t, 5>())); }\n", [],
     ["write of test::testreg (0x00000040) field(s) data is a read-modify-write",
      "key field(s) key (0xFFFF0000)"]),
    ("side-effect register, partial write",
     "void f() { apply(write(POPREG::data, R::value<std::uint32_t, 5>())); }\n", [],
     ["write of test::popreg (0x00000044) field(s) data", "reading it has a side effect"]),
    ("literal too wide, named register",
     "void f() { apply(write(CTRLREG::div, R::value<std::uint32_t, 20>())); }\n", [],
     ["literal 20 does not fit field(s) div of test::ctrlreg (0x00000048) (it holds 0..15)"]),
    ("literal too wide, unnamed register",
     "void f() { apply(write(PLAINREG::div, R::value<std::uint32_t, 20>())); }\n", [],
     ["literal 20 does not fit bits 7:4 of register 0x0000004C (it holds 0..15)"]),
    ("set on a wide field",
     "void f() { apply(set(CTRLREG::div)); }\n", [],
     ["Register::set only works on single bits, field(s) div of test::ctrlreg (0x00000048) is bits 7:4"]),
    ("a pin provided twice: the pin and a provider",
     """#include "kvasir/Io/Types.hpp"
struct LedA { using Provides = brigand::list<Kvasir::Io::PinResource<0, 25>>; };
struct LedB { using Provides = brigand::list<Kvasir::Io::PinResource<0, 25>>; };
using Check = Kvasir::Startup::ResourceCheck<brigand::list<LedA, LedB>>;
static_assert(Kvasir::Startup::Diagnostics::Diagnose<Check>::value);
""", [],
     # one report per provider: gcc prints both, clang stops after the first (LedB)
     ["a hardware resource is provided (configured, owned) by two peripherals: Io pin 0.25 - Led"]),
    ("an interrupt rule: the index and its name",
     """struct TwiceRule {
    static constexpr auto kind = Kvasir::Startup::Diagnostics::NumberKind::interrupt;
    static constexpr std::string_view message = "two peripherals claim the same interrupt vector";
};
struct TheStartup {};
constexpr auto n = sizeof(Kvasir::Startup::Diagnostics::Report<TwiceRule, TheStartup, std::integral_constant<int, -1>>);
""", [],
     ["two peripherals claim the same interrupt vector: interrupt -1 (SysTick) - TheStartup"]),
    ("an unclassified write-only bit not named",
     "void f() { apply(write(IcrstReg::latency, R::value<std::uint32_t, 5>())); }\n", [],
     ["it would write back the write-only field(s) bit 11 (0x00000800), which cannot be read"]),
    ("a toggle on a write-only register",
     "void f() { apply(toggle(SetResetReg::bs5)); }\n", [],
     ["a toggle needs the current value, and this register cannot be read"]),
    ("named diagnostics off: today's sentence",
     "void f() { apply(write(TESTREG::data, R::value<std::uint32_t, 5>())); }\n", [
         "-DKVASIR_NAMED_DIAGNOSTICS=0"],
     ["this write reads the register first (a read-modify-write), which the register does not allow"]),
]


def main() -> int:
    cxx, src, tests = sys.argv[1], sys.argv[2], sys.argv[3]
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, body, flags, want in CASES:
            f = Path(tmp) / "snippet.cpp"
            f.write_text(HEAD + body)
            r = subprocess.run([cxx, "-std=c++26", "-fsyntax-only", "-DKVASIR_REGISTER_MOCK", f"-I{src}", f"-I{tests}",
                                *flags, str(f)], capture_output=True, text=True)
            out = r.stdout + r.stderr
            missing = [w for w in want if w not in out]
            if r.returncode == 0 or missing:
                failed += 1
                print(
                    f"FAIL {name}: exit {r.returncode}, missing {missing}\n{out[:4000]}")
            else:
                print(f"ok   {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
