#!/usr/bin/env python3
"""cmake/tools/kvasir_postlink.py, the one post-link command of an image: the steps in their fixed order (checks, CRC,
artefacts, listing, size; image_crc only with --image-crc), a failing step ends the run with its own exit code and,
with --delete-on-failure, the ELF gone (tests/elf_image/image.S linked with lld has no ResetISR: reset_entry fails with
2 after ram_funcs passed), a tool's sys.exit("text") is its message and exit 1, a clean step prints nothing of the
driver's own. pass.S passes every step: the ELF is parsed once for all of them, the listing holds every executable
the image loads, read from the ELF (a chip's own .chip_code too, not a section that is not loaded) and the size output
is the memory table alone.
A missing toolchain is a skip.

    python3 tests/postlink/test_postlink.py -v
"""

import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
TOOL = SDK / 'cmake' / 'tools' / 'kvasir_postlink.py'
sys.path.insert(0, str(SDK / 'cmake' / 'tools'))
sys.path.insert(0, str(SDK / 'tests' / 'tooling'))
import armtc  # noqa: E402
import kvasir_postlink  # noqa: E402

ARGS = ['--linker-file', 'x.ld', '--objcopy',
        'llvm-objcopy', '--objdump', 'llvm-objdump']


def link(tmp, source):
    """source (an .S in tests/) assembled for the M0+ and linked with lld over tests/elf_image/flash.ld."""
    ld = tmp / 'flash.ld'
    ld.write_text((SDK / 'tests' / 'elf_image' /
                  'flash.ld').read_text().replace('FLASH_ORIGIN', '0x10000000'))
    obj, elf = tmp / 'image.o', tmp / 'image.elf'
    for cmd in (['clang', '--target=armv6m-none-eabi', '-c', str(source), '-o', str(obj)],
                ['ld.lld', '-T', str(ld), str(obj), '-o', str(elf)]):
        subprocess.run(cmd, check=True, capture_output=True)
    return ld, elf


class Postlink(unittest.TestCase):
    def test_order(self):
        def names(*extra):
            args = kvasir_postlink.argparse.Namespace(elf=Path('a.elf'), linker_file='x.ld', objcopy='o', objdump='d',
                                                      image_crc=bool(extra), exclude=[], uf2_family=None)
            return [name for (name, _step) in kvasir_postlink.steps(args, None)]
        self.assertEqual(
            names(), ['ram_funcs', 'reset_entry', 'undefined_refs', 'images', 'lst', 'size'])
        self.assertEqual(names('crc'), ['ram_funcs', 'reset_entry', 'undefined_refs', 'image_crc', 'images', 'lst',
                                        'size'])

    def test_run_tool(self):
        def tool(result):
            def run():
                if isinstance(result, str):
                    sys.exit(result)
                return result
            return run
        kvasir_postlink.run_tool(tool(0))
        kvasir_postlink.run_tool(tool(None))
        with self.assertRaises(kvasir_postlink.Failed) as failed:
            kvasir_postlink.run_tool(tool(2))
        self.assertEqual(failed.exception.code, 2)
        with self.assertRaises(kvasir_postlink.Failed) as failed:
            kvasir_postlink.run_tool(tool('tool.py: no symbol'))
        self.assertEqual(failed.exception.code, 1)

    def test_listing_sections(self):
        pieces = [('.boot2', 0x10000000, b''), ('.vectors', 0x10000100, b''), ('.text', 0x10000200, b''),
                  ('.text', 0x10004000, b''), ('.core_reset_guard', 0x10008000, b'')]
        elf = types.SimpleNamespace(loaded=lambda: pieces)
        self.assertEqual(kvasir_postlink.listing_sections(elf), [
                         '.boot2', '.vectors', '.text', '.core_reset_guard'])

    def test_one_parse_and_every_step(self):
        armtc.need(self, 'clang', 'ld.lld', 'llvm-objcopy', 'llvm-objdump')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            ld, elf = link(tmp, HERE / 'pass.S')
            parsed = []
            original = kvasir_postlink.Elf.__init__

            def counting(self_, data):
                parsed.append(len(data))
                original(self_, data)
            kvasir_postlink.Elf.__init__ = counting
            try:
                code = kvasir_postlink.main([str(elf), '--linker-file', str(ld), '--objcopy', 'llvm-objcopy',
                                             '--objdump', 'llvm-objdump'])
            finally:
                kvasir_postlink.Elf.__init__ = original
            self.assertEqual(code, 0)
            # the image once, for the driver and every step; elf_image's _flash.elf (another, smaller file) once
            size = elf.stat().st_size
            self.assertEqual(parsed.count(
                size), 1, 'the driver and every step on one parse of the ELF')
            self.assertEqual(
                len([n for n in parsed if n != size]), 1, '_flash.elf parsed once')
            listing = (tmp / 'image.lst').read_text()
            self.assertIn('Disassembly of section .text', listing)
            self.assertIn('Disassembly of section .chip_code', listing)
            self.assertIn('Disassembly of section .data', listing)
            self.assertNotIn('.kvasir_meta', listing)
            for suffix in ('_flash.bin', '_flash.hex', '_flash.elf'):
                self.assertTrue((tmp / f'image{suffix}').exists(), suffix)

    def test_size_is_the_memory_table_alone(self):
        armtc.need(self, 'clang', 'ld.lld', 'llvm-objcopy', 'llvm-objdump')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            ld, elf = link(tmp, HERE / 'pass.S')
            result = subprocess.run([sys.executable, '-B', str(TOOL), str(elf), '--linker-file', str(ld),
                                     '--objcopy', 'llvm-objcopy', '--objdump', 'llvm-objdump'],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            lines = result.stdout.splitlines()
            self.assertTrue(lines and lines[0].split()[:2] == [
                            'Memory', 'region'], result.stdout)
            # size --format=sysv's last line
            self.assertNotIn('Total', result.stdout)

    def test_failure_deletes_the_elf(self):
        armtc.need(self, 'clang', 'ld.lld')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            _ld, elf = link(tmp, SDK / 'tests' / 'elf_image' / 'image.S')
            for (flag, gone) in (([], False), (['--delete-on-failure'], True)):
                with self.subTest(flag=flag):
                    result = subprocess.run([sys.executable, '-B', str(TOOL), str(elf), *ARGS, *flag],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn('no symbol ResetISR', result.stderr)
                    self.assertNotIn(
                        ': warning:', result.stdout + result.stderr)
                    self.assertEqual(elf.exists(), not gone)
                    self.assertEqual(
                        'removed image.elf' in result.stderr, gone)


if __name__ == '__main__':
    unittest.main()
