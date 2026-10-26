#!/usr/bin/env python3
"""cmake/tools/ram_image_jlink.py, the J-Link script that starts a RAM_ONLY image without a reset: links image.S with
clang + ld.lld over ram.ld and checks the script it writes from a template (the table address, SP = word 0, PC = word 1
without the Thumb bit), and its refusals: a symbol missing, a misaligned SP, a file that is no ELF, the usage. A
missing toolchain is a skip, and a failure for a tool group KVASIR_TEST_REQUIRE names (tests/tooling/armtc.py).

    python3 tests/ram_image_jlink/test_ram_image_jlink.py -v
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

TOOL = HERE.parent.parent / 'cmake' / 'tools' / 'ram_image_jlink.py'
TEMPLATE = 'w4 0xE000ED08 {vector_table}\nSetSP {initial_sp}\nSetPC {reset_pc}\nGo\n'


class RamImageJlink(unittest.TestCase):
    def setUp(self):
        armtc.need(self, 'clang', 'ld.lld')
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / 'template.jlink').write_text(TEMPLATE)

    def tearDown(self):
        self.tmp.cleanup()

    def link(self, name, *defines, script=HERE / 'ram.ld'):
        obj, elf = self.dir / f'{name}.o', self.dir / f'{name}.elf'
        subprocess.run(['clang', '--target=armv6m-none-eabi', *defines, '-c', str(HERE / 'image.S'), '-o', str(obj)],
                       check=True, capture_output=True)
        subprocess.run(['ld.lld', '-T', str(script), str(obj),
                       '-o', str(elf)], check=True, capture_output=True)
        return elf

    def run_tool(self, elf, *extra):
        out = self.dir / 'out.jlink'
        result = subprocess.run([sys.executable, '-B', str(TOOL), str(elf), str(self.dir / 'template.jlink'),
                                 str(out), *extra], capture_output=True, text=True)
        return result, out

    def test_script(self):
        result, out = self.run_tool(self.link('good'))
        self.assertEqual(result.returncode, 0, result.stderr)
        # .vectors at the start of RAM, .text right after the two words: entry at 0x20000008
        self.assertEqual(out.read_text(
        ), 'w4 0xE000ED08 0x20000000\nSetSP 0x20001000\nSetPC 0x20000008\nGo\n')

    def test_misaligned_sp(self):
        result, out = self.run_tool(self.link('bad', '-DBAD_SP'))
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            'implausible vector table at 0x20000000: SP 0x20001002', result.stderr)
        self.assertFalse(out.exists())

    def test_no_symbol(self):
        script = self.dir / 'nosym.ld'
        script.write_text(
            (HERE / 'ram.ld').read_text().replace('_LINKER_vectors_start_ = .; ', ''))
        result, _out = self.run_tool(self.link('nosym', script=script))
        self.assertEqual(result.returncode, 1)
        self.assertIn('no symbol _LINKER_vectors_start_', result.stderr)

    def test_not_an_elf(self):
        junk = self.dir / 'junk.elf'
        junk.write_bytes(b'not an elf at all')
        result, _out = self.run_tool(junk)
        self.assertEqual(result.returncode, 1)
        self.assertIn('not a 32-bit little-endian ELF', result.stderr)

    def test_usage(self):
        result = subprocess.run(
            [sys.executable, '-B', str(TOOL), 'only-one'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('usage: ram_image_jlink.py', result.stderr)


if __name__ == '__main__':
    unittest.main()
