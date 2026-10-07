#!/usr/bin/env python3
"""size_report.py: name bucketing, the parsers on recorded tool output, diff arithmetic, and one
snapshot of an image linked here (skipped without clang and ld.lld)."""
import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True  # no __pycache__ next to the imported tools

HERE = Path(__file__).resolve().parent

spec = importlib.util.spec_from_file_location(
    "size_report", HERE / "size_report.py")
assert spec is not None and spec.loader is not None
sr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sr)


class Names(unittest.TestCase):
    def test_plain_name(self):
        for name, plain in [
            ("memcpy", "memcpy"),
            ("void a::B<c<int>, 3>::f<x>(d<e>, int) const", "a::B::f"),
            ("(anonymous namespace)::f(int)", "(anonymous namespace)::f"),
            ("a::B<int>::operator()(int) const", "a::B::operator()"),
            ("bool operator<(A const&, A const&)", "operator<"),
            ("Kvasir::Startup::StartupImpl<X<1>>::ResetISR()",
             "Kvasir::Startup::StartupImpl::ResetISR"),
            ("main::{lambda(int)#1}::operator()(int) const",
             "main::{lambda(int)#1}::operator()"),
            ("std::__2::array<int, 3ul> x", "x"),
        ]:
            self.assertEqual(sr.plain_name(name), plain, name)

    def test_buckets(self):
        for name, bucket in [
            ("__llvm_libc::inline_memcpy(char*, char const*, unsigned int)", "libc"),
            ("memset", "libc"),
            ("__aeabi_memcpy4", "libc"),
            ("__aeabi_uidivmod", "compiler-rt"),
            ("__udivmoddi4", "compiler-rt"),
            ("__aeabi_uwrite4", "compiler-rt"),
            ("divmod_u32u32", "compiler-rt"),
            ("std::__2::to_chars(char*, char*, int)", "libc++"),
            ("void uc_log::detail::Log<std::__2::tuple<int>>::log<B, 'a'>(int)", "logging"),
            ("remote_fmt::Printer<X>::append<unsigned int>(unsigned int)", "logging"),
            ("Kvasir::Startup::StartupImpl<X>::ResetISR()", "startup"),
            ("__stack_chk_fail", "startup"),
            ("Kvasir::USB::Device<X>::poll()", "usb"),
            ("Kvasir::Atomic::Queue<char, 64ul>::push(char)", "kvasir"),
            ("OUTLINED_FUNCTION_12", "outlined"),
            ("__Thumbv6MABSLongThunk___atomic_load_4", "thunk"),
            ("main", "app"),
            # a template argument from another namespace does not decide the bucket
            ("App::Handler<Kvasir::USB::Device<X>>::run()", "app"),
        ]:
            self.assertEqual(sr.bucket_of(name), bucket, name)


SECTIONS = """\
Section Headers:
  [Nr] Name              Type            Address  Off    Size   ES Flg Lk Inf Al
  [ 0]                   NULL            00000000 000000 000000 00      0   0  0
  [ 1] .boot2            PROGBITS        10000000 001000 000100 00   A  0   0  1
  [ 2] .vectors          PROGBITS        10000100 001100 0000a8 00   A  0   0  4
  [ 3] .text             PROGBITS        100001a8 0011a8 000040 00  AX  0   0  4
  [ 4] .stack            NOBITS          20000000 002000 03ff00 00  WA  0   0  8
  [ 5] .data             PROGBITS        2003ff00 002f00 000008 00 WAX  0   0  4
  [ 6] .bss              NOBITS          2003ff08 002f08 000020 00  WA  0   0  4
  [ 7] .noInit           NOBITS          2003ff28 002f08 000010 00  WA  0   0  4
  [ 8] .debug_info       PROGBITS        00000000 002f08 001234 00      0   0  1
  [ 9] remote_fmt_sites  PROGBITS        00000000 004200 000010 00      0   0  1
"""

SYMBOLS = """\
Symbol table '.symtab' contains 9 entries:
   Num:    Value  Size Type    Bind   Vis       Ndx Name
     0: 00000000     0 NOTYPE  LOCAL  DEFAULT   UND
     1: 100001a9    16 FUNC    LOCAL  DEFAULT     3 memcpy
     2: 100001b9    12 FUNC    GLOBAL DEFAULT     3 __udivsi3
     3: 100001b9    12 FUNC    GLOBAL DEFAULT     3 __aeabi_uidiv
     4: 100001c5    20 FUNC    GLOBAL DEFAULT     3 main
     5: 100001d8     8 OBJECT  LOCAL  DEFAULT     3 App::table
     6: 2003ff00     8 OBJECT  LOCAL  DEFAULT     5 App::state
     7: 2003ff08    32 OBJECT  LOCAL  DEFAULT     6 Kvasir::Atomic::Queue<char, 32ul>::buf
     8: 00000001     0 NOTYPE  LOCAL  DEFAULT   ABS SOME_CONSTANT
     9: 00000000     4 OBJECT  LOCAL  DEFAULT     9 site_tag
"""

DISASSEMBLY = """\
100001a8 <memcpy>:
100001a8:       bl      0x100001f0 <__aeabi_uread4>       @ imm = #0x44
100001ac:       b       0x100001a8 <memcpy>       @ imm = #-0x8

100001c4 <main>:
100001c4:       bl      0x100001a8 <memcpy>       @ imm = #-0x20
100001c8:       bl      0x100001f8 <__atomic_load_4>       @ imm = #0x2c
100001cc:       bl      0x100001f8 <__atomic_load_4>       @ imm = #0x28
100001d0:       bl      0x100001b8 <__aeabi_uidiv>       @ imm = #-0x1c
100001d4:       bl      0x10000200 <__stack_chk_fail>       @ imm = #0x28
100001d6:       bl      0x10000204 <main+0x40>       @ imm = #0x28
100001d8:       .word   0x2003ff00
"""


class Parsers(unittest.TestCase):
    def test_sections_keep_only_allocated(self):
        sections = sr.parse_sections(SECTIONS)
        self.assertEqual([s["name"] for s in sections.values()],
                         [".boot2", ".vectors", ".text", ".stack", ".data", ".bss", ".noInit"])
        self.assertEqual(sections[3]["size"], 0x40)

    def test_symbols(self):
        symbols = sr.parse_symbols(SYMBOLS)
        self.assertEqual(symbols[0], (0x100001a8, 16,
                         "FUNC", 3, "memcpy"))   # Thumb bit off
        self.assertNotIn("SOME_CONSTANT", [s[4] for s in symbols])

    def test_calls(self):
        calls = sr.parse_calls(DISASSEMBLY)
        self.assertEqual(calls["atomic"], 2)
        self.assertEqual(calls["unaligned"], 1)
        self.assertEqual(calls["div"], 1)
        # main's call; memcpy's own loop branch is none
        self.assertEqual(calls["memcpy"], 1)
        self.assertEqual(calls["canary_functions"], 1)
        self.assertEqual(calls["detail"], {"__aeabi_uidiv": 1, "__aeabi_uread4": 1,
                                           "__atomic_load_4": 2})

    def test_analyse(self):
        image = sr.analyse(sr.parse_sections(SECTIONS), sr.parse_symbols(SYMBOLS),
                           sr.parse_calls(DISASSEMBLY))
        self.assertEqual(image["flash"], 0x100 + 0xa8 + 0x40 + 8)
        self.assertEqual(image["ram"], 8 + 0x20 + 0x10)           # no .stack
        self.assertEqual(image["buckets"]["libc"], {"code": 16, "data": 0})
        self.assertEqual(image["buckets"]["compiler-rt"]
                         ["code"], 12)   # the alias counts once
        self.assertEqual(image["buckets"]["app"], {"code": 20, "data": 16})
        self.assertEqual(image["unnamed"],
                         image["flash"] - (16 + 12 + 20 + 8 + 8))
        self.assertEqual(list(image["ram_objects"].items())[0],
                         (".bss Kvasir::Atomic::Queue<char, 32ul>::buf", 32))
        # not a loaded section
        self.assertNotIn("site_tag", image["symbols"])


def image(flash: int, symbols: dict[str, int], atomic: int = 0) -> dict:
    calls = {name: 0 for name, _ in sr.CALLS} | {
        "canary_functions": 0, "detail": {}}
    calls["atomic"] = atomic
    buckets = {b: {"code": 0, "data": 0} for b in sr.BUCKET_NAMES}
    buckets["app"]["code"] = flash
    return {"flash": flash, "ram": 0, "sections": {".text": flash}, "buckets": buckets,
            "unnamed": 0, "calls": calls, "ram_objects": {}, "symbols": symbols}


class Diff(unittest.TestCase):
    def test_diff(self):
        with tempfile.TemporaryDirectory() as tmp:
            before, after = Path(tmp) / "a.json", Path(tmp) / "b.json"
            before.write_text(json.dumps({"images": {
                "t/x_release": image(1000, {"f()": 600, "g()": 400}, atomic=5),
                "t/same_release": image(500, {"h()": 500}),
                "t/gone_release": image(1, {})}}))
            after.write_text(json.dumps({"images": {
                "t/x_release": image(900, {"f()": 500, "g()": 400}, atomic=0),
                "t/same_release": image(500, {"h()": 500})}}))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                sr.cmd_diff(SimpleNamespace(before=str(before), after=str(after), filter=None,
                                            all=False, top=5))
        text = out.getvalue()
        self.assertIn(
            "2 images in both, 1 differ; only in before: 1, only in after: 0", text)
        self.assertIn("flash total 1,500 -> 1,400 (-100, -6.67 %)", text)
        self.assertIn("| t/x_release | 1,000 | 900 | -100 | -10.0 % | 0 | app -100 | atomic -5 |",
                      text)
        self.assertNotIn("t/same_release", text)
        self.assertIn("| f() | -100 |", text)


SOURCE = """
extern int counter;
int counter = 3;
char buffer[64];
static const int table[4] = {1, 2, 3, 4};
__attribute__((noinline)) int helper(int i) { return table[i & 3] + counter; }
int entry(void) { buffer[0] = (char)helper(1); return buffer[0]; }
"""


@unittest.skipUnless(shutil.which("clang") and shutil.which("ld.lld")
                     and shutil.which("llvm-readelf") and shutil.which("llvm-objdump"),
                     "needs clang, ld.lld, llvm-readelf and llvm-objdump")
class LinkedImage(unittest.TestCase):
    def test_snapshot_of_a_linked_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            tree = Path(tmp) / "repo" / "build"
            tree.mkdir(parents=True)
            (tree / "x.c").write_text(SOURCE)
            (tree / "x.ld").write_text(
                "SECTIONS { .text 0x10000000 : { *(.text*) *(.rodata*) }\n"
                " .data 0x20000000 : { *(.data*) } .bss : { *(.bss*) *(COMMON) } }\n")
            subprocess.run(["clang", "-target", "thumbv6m-none-eabi", "-mcpu=cortex-m0plus",
                            "-Oz", "-ffreestanding", "-c", "x.c", "-o", "x.o"],
                           cwd=tree, check=True)
            subprocess.run(["ld.lld", "--script=x.ld", "--entry=entry", "x.o", "-o",
                            "demo_release.elf"], cwd=tree, check=True)
            shutil.copy(tree / "demo_release.elf",
                        tree / "demo_release_flash.elf")
            shutil.copy(tree / "demo_release.elf", tree / "demo_debug.elf")
            out = Path(tmp) / "snap.json"
            with contextlib.redirect_stdout(io.StringIO()):
                sr.cmd_snapshot(SimpleNamespace(trees=[str(tree)], output=str(out),
                                                variants="release,release_log", jobs=2,
                                                no_symbols=False))
            images = json.loads(out.read_text())["images"]
        # no _flash, no debug
        self.assertEqual(list(images), ["repo/build/demo_release"])
        im = images["repo/build/demo_release"]
        self.assertEqual(im["symbols"]["buffer"], 64)
        self.assertEqual(im["symbols"]["table"], 16)
        self.assertEqual(im["ram"], 4 + 64)
        self.assertEqual(im["flash"], im["sections"]
                         [".text"] + im["sections"][".data"])
        # table in .text, and counter's initial value: .data's load image is flash
        self.assertEqual(im["buckets"]["app"]["data"], 16 + 4)
        self.assertGreater(im["buckets"]["app"]["code"], 0)
        self.assertEqual(im["ram_objects"][".bss buffer"], 64)


if __name__ == "__main__":
    unittest.main()
