#!/usr/bin/env python3
"""The stack fills the RAM the other sections leave, in one link (linker/common_stack_body.inc.ld).

Links image.S with the SDK's real linker scripts, through three chip-shaped scripts here (a flash image, a RAM-only
image, a RAM-only image with a section no script names), with ld.lld and arm-none-eabi-ld where installed, and holds
the stack to the formula, computed here from the image's own sections:

    extra = ram - (sizes of the ram sections but .stack and .heap + min stack + heap + ram // 256)
    stack end = align8(end of the stack protector + min stack + extra - stack protector)

and checks that a heap or minimum stack that does not fit fails the link with a sentence.

    python3 tests/stack_fill/test_stack_fill.py -v
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
LINKER_DIR = HERE.parent.parent / 'linker'

PROTECTOR = 32   # image.S's .stackProtector

LAYOUTS = {
    # script, RAM size, BOOT2 assembled in
    'flash': ('flash.ld', 16 * 1024, False),
    'ram_only': ('ram_only.ld', 32 * 1024, False),
    'ram_only_orphan': ('ram_only_orphan.ld', 32 * 1024, True),
    # the same script for an image without the section: it takes no room
    'ram_only_orphan_absent': ('ram_only_orphan.ld', 32 * 1024, False),
}

LINKERS = {
    'lld': ['ld.lld'],
    'gnu': ['arm-none-eabi-ld'],
}


def align8(n):
    return (n + 7) // 8 * 8


class StackFill(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        cls.objects = {}
        if shutil.which('clang'):
            assembler = ['clang', '--target=thumbv7m-none-eabi', '-c']
        elif shutil.which('arm-none-eabi-gcc'):
            assembler = ['arm-none-eabi-gcc',
                         '-mcpu=cortex-m3', '-mthumb', '-c']
        else:
            raise unittest.SkipTest(
                'no ARM assembler (clang or arm-none-eabi-gcc)')
        for boot2 in (False, True):
            obj = cls.tmp / f'image{"_boot2" if boot2 else ""}.o'
            subprocess.run(assembler + (['-DBOOT2'] if boot2 else []) +
                           [str(HERE / 'image.S'), '-o', str(obj)], check=True)
            cls.objects[boot2] = obj

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def link(self, linker, layout, min_stack, heap):
        if shutil.which(LINKERS[linker][0]) is None:
            self.skipTest(f'{LINKERS[linker][0]} not found')
        script, ram, boot2 = LAYOUTS[layout]
        elf = self.tmp / f'{linker}_{layout}_{min_stack}_{heap}.elf'
        cmd = LINKERS[linker] + [
            f'--library-path={LINKER_DIR}',
            f'--defsym=cmake_ram_size={ram}',
            f'--defsym=cmake_min_stack_size={min_stack}',
            f'--defsym=cmake_heap_size={heap}',
            '--defsym=cmake_core1_stack_size=0',
            '--defsym=cmake_core1_stack_in_scratch=0',
            '--defsym=cmake_scratch_banks=0',
            f'--script={HERE / script}', '--entry=ResetISR',
            str(self.objects[boot2]), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result, elf, ram

    def sections(self, elf):
        """name -> (size, address) from the linked image"""
        tool = shutil.which('llvm-size') or shutil.which('arm-none-eabi-size')
        if tool is None:
            self.skipTest('no llvm-size / arm-none-eabi-size')
        out = subprocess.run([tool, '-A', '-d', str(elf)],
                             capture_output=True, text=True, check=True).stdout
        found = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0].startswith('.') and parts[1].isdigit():
                found[parts[0]] = (int(parts[1]), int(parts[2]))
        return found

    def symbols(self, elf):
        tool = shutil.which('llvm-nm') or shutil.which('arm-none-eabi-nm')
        if tool is None:
            self.skipTest('no llvm-nm / arm-none-eabi-nm')
        out = subprocess.run(
            [tool, str(elf)], capture_output=True, text=True, check=True).stdout
        return {parts[2]: int(parts[0], 16) for parts in (line.split() for line in out.splitlines()) if len(parts) == 3}

    def expect_fill(self, linker, layout, min_stack, heap):
        result, elf, ram = self.link(linker, layout, min_stack, heap)
        self.assertEqual(result.returncode, 0, result.stderr)
        secs = self.sections(elf)
        ram_start, ram_end = 0x20000000, 0x20000000 + ram
        others = sum(size for name, (size, addr) in secs.items()
                     if ram_start <= addr < ram_end and name not in ('.stack', '.heap'))
        extra = ram - (others + min_stack + heap + ram // 256)
        self.assertGreater(extra, 0)
        syms = self.symbols(elf)
        self.assertEqual(syms['_LINKER_INTERN_stackProtector_end_'] -
                         syms['_LINKER_INTERN_stack_start_'], PROTECTOR)
        self.assertEqual(syms['_LINKER_INTERN_stack_end_'],
                         align8(
                             syms['_LINKER_INTERN_stackProtector_end_'] + min_stack + extra - PROTECTOR),
                         f'{layout}: others {others}')
        self.assertEqual(secs['.heap'][0], heap)
        # what is left above the last section is the margin, give or take the alignment
        top = max(addr + size for size, addr in secs.values()
                  if ram_start <= addr < ram_end)
        self.assertLessEqual(top, ram_end)
        self.assertLess(ram_end - top, ram // 256 + 64)

    def expect_refused(self, linker, layout, min_stack, heap):
        result, _, _ = self.link(linker, layout, min_stack, heap)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not enough RAM', result.stderr)

    def test_fills(self):
        for linker in LINKERS:
            for layout in LAYOUTS:
                for min_stack, heap in ((1024, 0), (2048, 4096), (4000, 1000)):
                    with self.subTest(linker=linker, layout=layout, min_stack=min_stack, heap=heap):
                        self.expect_fill(linker, layout, min_stack, heap)

    def test_heap_too_big(self):
        for linker in LINKERS:
            for layout in LAYOUTS:
                with self.subTest(linker=linker, layout=layout):
                    self.expect_refused(
                        linker, layout, 1024, LAYOUTS[layout][1])

    def test_margin_does_not_fit(self):
        # everything fits in RAM, the RAM/256 margin does not: refused
        for linker in LINKERS:
            for layout in LAYOUTS:
                with self.subTest(linker=linker, layout=layout):
                    result, elf, ram = self.link(linker, layout, 1024, 0)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    secs = self.sections(elf)
                    others = sum(size for name, (size, addr) in secs.items()
                                 if 0x20000000 <= addr < 0x20000000 + ram and name not in ('.stack', '.heap'))
                    heap = ram - others - 1024 - ram // 256 + 16
                    self.expect_refused(linker, layout, 1024, heap)


if __name__ == '__main__':
    unittest.main()
