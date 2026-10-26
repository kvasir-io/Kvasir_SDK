#!/usr/bin/env python3
"""cmake/tools/pretty_size.py, the per-link memory table: links tests/elf_image/image.S with every ARM toolchain found
(clang + ld.lld, arm-none-eabi-as + GNU ld) into a flash image (size.ld: .data and .trap_data run in RAM and are
loaded from flash, an EEPROM region, a NOLOAD buffer, a non-ALLOC section at address 0) and a RAM image
(tests/elf_image/ram.ld), runs the script on each and checks the rows: flash counts every section's load image, a RAM
row every section that runs there, eeprom its section, the RAM image every section once. A missing toolchain is a
skip, and a failure for a tool group KVASIR_TEST_REQUIRE names (tests/tooling/armtc.py).

    python3 tests/pretty_size/test_pretty_size.py -v
"""

import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

TOOLS = HERE.parent.parent / 'cmake' / 'tools'
SCRIPT = TOOLS / 'pretty_size.py'
IMAGE_S = HERE.parent / 'elf_image' / 'image.S'
RAM_LD = HERE.parent / 'elf_image' / 'ram.ld'

spec = importlib.util.spec_from_file_location(
    'kvasir_elf', TOOLS / 'kvasir_elf.py')
kvasir_elf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kvasir_elf)

TOOLCHAINS = {
    'llvm': (['clang', '--target=armv6m-none-eabi', '-c'], ['ld.lld']),
    'gnu': (['arm-none-eabi-gcc', '-mcpu=cortex-m0plus', '-mthumb', '-c'], ['arm-none-eabi-ld']),
}
ROW = re.compile(
    r'^\s*(\w+)\s+(\d+(?:\.\d+)?)\s+(B|KB|MB)\s+\S+\s+(?:B|KB|MB)\s+\S+%$')


def rows(text):
    """{region: used bytes} of the table (every value here is below 64 KiB, so printed in bytes)."""
    out = {}
    for line in text.splitlines():
        if (m := ROW.match(line)):
            assert m.group(3) == 'B', line
            out[m.group(1)] = int(m.group(2))
    return out


class PrettySize(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def link(self, toolchain, script):
        assembler, linker = TOOLCHAINS[toolchain]
        armtc.need(self, assembler[0], linker[0])
        elf = Path(self.tmp.name) / f'{toolchain}_{script.stem}.elf'
        obj = elf.with_suffix('.o')
        for cmd in (assembler + [str(IMAGE_S), '-o', str(obj)],
                    linker + ['-T', str(script), str(obj), '-o', str(elf)]):
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0,
                             f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def table(self, elf, script):
        # argv as kvasir_postlink.py passes it: <elf> <linker file>
        result = subprocess.run([sys.executable, '-B', str(SCRIPT), str(elf), str(script)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        return rows(result.stdout)

    @staticmethod
    def sizes(elf):
        return {s['name']: s['size'] for s in kvasir_elf.Elf(elf.read_bytes()).sections}

    def check_flash_image(self, toolchain):
        script = HERE / 'size.ld'
        elf = self.link(toolchain, script)
        size = self.sizes(elf)
        got = self.table(elf, script)
        self.assertEqual(set(got), {'flash', 'eeprom', 'ram'})
        self.assertEqual(got['ram'], sum(size[s] for s in (
            '.data', '.trap_data', '.noload_buffer', '.bss')))
        self.assertEqual(got['eeprom'], 8)
        # the row the VMA-only booking got wrong: .data and .trap_data are loaded from flash too
        self.assertEqual(got['flash'], sum(size[s]
                         for s in ('.vectors', '.text', '.data', '.trap_data')))

    def check_ram_image(self, toolchain):
        elf = self.link(toolchain, RAM_LD)
        size = self.sizes(elf)
        got = self.table(elf, RAM_LD)
        self.assertEqual(got, {'ram': sum(size[s] for s in ('.vectors', '.text', '.noload_buffer', '.data',
                                                            '.trap_data', '.bss'))})

    def test_flash_image_lld(self):
        self.check_flash_image('llvm')

    def test_flash_image_gnu_ld(self):
        self.check_flash_image('gnu')

    def test_ram_image_lld(self):
        self.check_ram_image('llvm')

    def test_ram_image_gnu_ld(self):
        self.check_ram_image('gnu')

    def test_unreadable_elf_fails(self):
        bogus = Path(self.tmp.name) / 'not_an.elf'
        bogus.write_bytes(b'not an ELF')
        result = subprocess.run([sys.executable, '-B', str(SCRIPT), str(bogus), str(HERE / 'size.ld')],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('not_an.elf', result.stderr)


if __name__ == '__main__':
    unittest.main()
