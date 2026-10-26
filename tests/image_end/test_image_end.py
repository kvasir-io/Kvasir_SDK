#!/usr/bin/env python3
"""`_LINKER_image_end_flash_` is the end of the flash image (linker/common.ld), and cmake/tools/elf_image.py refuses an
image that loads a flash section past it.

Links image.S with the SDK's real linker scripts through four chip-shaped scripts here, with ld.lld and
arm-none-eabi-ld where installed:

    flash.ld              nothing after .data: the symbol is the common.ld default, .data's load end
    flash_tail.ld         a section of the chip's own loaded after .data, the image end moved past it before common.ld;
                          with the section empty (no TAIL in the image) the end is .data's again
    flash_tail_unmoved.ld the same without the image end line: the symbol stays at .data's end
    ram_only.ld           a RAM-only image: 0

For every flash image the symbol must equal the end of the last flash PT_LOAD with file contents (read with
llvm-readelf / arm-none-eabi-readelf -l), elf_image.py must pass it, and for flash_tail_unmoved.ld with TAIL it must
fail naming .tail and the bytes past the end.

    python3 tests/image_end/test_image_end.py -v
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

LINKER_DIR = HERE.parent.parent / 'linker'
ELF_IMAGE = HERE.parent.parent / 'cmake' / 'tools' / 'elf_image.py'

FLASH = (0x10000000, 0x10000000 + 64 * 1024)
TAIL_BYTES = 164   # image.S's .tail.data

LINKERS = {
    'lld': ['ld.lld'],
    'gnu': ['arm-none-eabi-ld'],
}


class ImageEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.objects = {}
        # a missing tool of a group KVASIR_TEST_REQUIRE names fails here instead of skipping (tests/tooling/armtc.py)
        if Path(armtc.first(None, 'clang', 'arm-none-eabi-gcc')).name == 'clang':
            assembler = ['clang', '--target=thumbv7m-none-eabi', '-c']
        else:
            assembler = ['arm-none-eabi-gcc',
                         '-mcpu=cortex-m3', '-mthumb', '-c']
        for tail in (False, True):
            obj = cls.tmp / f'image{"_tail" if tail else ""}.o'
            subprocess.run(assembler + (['-DTAIL'] if tail else []) + [str(HERE / 'image.S'), '-o', str(obj)],
                           check=True)
            cls.objects[tail] = obj
        cls.readelf = armtc.first(
            None, 'llvm-readelf', 'arm-none-eabi-readelf')
        cls.nm = armtc.first(None, 'llvm-nm', 'arm-none-eabi-nm')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def link(self, linker, script, tail):
        armtc.need(self, LINKERS[linker][0])
        elf = self.tmp / \
            f'{linker}_{Path(script).stem}{"_tail" if tail else ""}.elf'
        cmd = LINKERS[linker] + [
            f'--library-path={LINKER_DIR}',
            '--defsym=cmake_min_stack_size=1024',
            '--defsym=cmake_heap_size=0',
            '--defsym=cmake_core1_stack_size=0',
            '--defsym=cmake_core1_stack_in_scratch=0',
            '--defsym=cmake_scratch_banks=0',
            f'--script={HERE / script}', '--entry=ResetISR',
            str(self.objects[tail]), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def symbol(self, elf, name):
        out = subprocess.run(
            [self.nm, str(elf)], capture_output=True, text=True, check=True).stdout
        found = [int(parts[0], 16) for parts in (line.split() for line in out.splitlines())
                 if len(parts) == 3 and parts[2] == name]
        self.assertEqual(len(found), 1, f'{name} in {elf.name}')
        return found[0]

    def last_flash_load_end(self, elf):
        """The end of the last PT_LOAD with file contents whose physical address lies in flash"""
        out = subprocess.run([self.readelf, '-lW', str(elf)],
                             capture_output=True, text=True, check=True).stdout
        ends = []
        for line in out.splitlines():
            parts = line.split()
            if parts and parts[0] == 'LOAD':
                (paddr, filesz) = (int(parts[3], 16), int(parts[4], 16))
                if filesz and FLASH[0] <= paddr < FLASH[1]:
                    ends.append(paddr + filesz)
        self.assertTrue(ends, f'no flash PT_LOAD in {elf.name}')
        return max(ends)

    def elf_image(self, elf):
        return subprocess.run([sys.executable, '-B', str(ELF_IMAGE), str(elf), '--out-prefix',
                               str(elf.with_suffix(''))], capture_output=True, text=True)

    def test_flash_default(self):
        for linker in LINKERS:
            with self.subTest(linker=linker):
                elf = self.link(linker, 'flash.ld', False)
                end = self.symbol(elf, '_LINKER_image_end_flash_')
                self.assertEqual(end, self.symbol(
                    elf, '_LINKER_data_end_flash_'))
                self.assertEqual(end, self.last_flash_load_end(elf))
                result = self.elf_image(elf)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_flash_tail(self):
        for linker in LINKERS:
            for tail in (True, False):
                with self.subTest(linker=linker, tail=tail):
                    elf = self.link(linker, 'flash_tail.ld', tail)
                    end = self.symbol(elf, '_LINKER_image_end_flash_')
                    data_end = self.symbol(elf, '_LINKER_data_end_flash_')
                    self.assertEqual(end, self.last_flash_load_end(elf))
                    # an empty .tail takes no room, wherever its LOADADDR is
                    self.assertEqual(end - data_end, TAIL_BYTES if tail else 0)
                    result = self.elf_image(elf)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_flash_tail_unmoved(self):
        for linker in LINKERS:
            for tail in (True, False):
                with self.subTest(linker=linker, tail=tail):
                    elf = self.link(linker, 'flash_tail_unmoved.ld', tail)
                    end = self.symbol(elf, '_LINKER_image_end_flash_')
                    self.assertEqual(end, self.symbol(
                        elf, '_LINKER_data_end_flash_'))
                    result = self.elf_image(elf)
                    if not tail:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        continue
                    self.assertNotEqual(end, self.last_flash_load_end(elf))
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(f'section .tail is loaded at {end:#010x}..{end + TAIL_BYTES:#010x}, {TAIL_BYTES} '
                                  f'bytes past _LINKER_image_end_flash_ ({end:#010x})', result.stderr)

    def test_ram_only(self):
        for linker in LINKERS:
            with self.subTest(linker=linker):
                elf = self.link(linker, 'ram_only.ld', False)
                self.assertEqual(self.symbol(
                    elf, '_LINKER_image_end_flash_'), 0)
                result = self.elf_image(elf)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
