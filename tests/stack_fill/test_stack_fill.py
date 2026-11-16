#!/usr/bin/env python3
"""The stack fills the RAM the other sections leave, in one link (linker/common_stack_body.inc.ld).

Links image.S with the SDK's real linker scripts, through three chip-shaped scripts here (a flash image, a RAM-only
image, a RAM-only image with a section no script names), with ld.lld and arm-none-eabi-ld where installed, and holds
the stack to the formula, computed here from the image's own sections:

    margin = max(sum of the ram sections' alignments + 24, ram // 256)
    extra = ram - (sizes of the ram sections but .stack and .heap + min stack + heap + margin)
    stack end = align8(end of the stack protector + min stack + extra - stack protector)

The margin is the most the padding between the sections can take (less than each one's alignment, and the stack
body's three ALIGN(8) lines); an image with a 2 KiB-aligned .bss input links with it where RAM // 256 alone overflowed.

and checks that a heap or minimum stack that does not fit fails the link with a sentence.

    python3 tests/stack_fill/test_stack_fill.py -v
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'tests' / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure
LINKER_DIR = HERE.parent.parent / 'linker'

PROTECTOR = 32   # image.S's .stackProtector

# The sections each script family counts in its alignment margin (common_ram.ld / common_ram_only.ld)
FLASH_PADDED = ('.noInitLowRam', '.stack', '.stack1', '.data',
                '.ramExec', '.bss', '.noInit', '.heap')
RAM_ONLY_PADDED = ('.vectors', '.noInitLowRam', '.text', '.ramExec', '.data', '.bss', '.noInit', '.stack', '.stack1',
                   '.heap')

LAYOUTS = {
    # script, RAM size, assembler defines, sections in the margin
    'flash': ('flash.ld', 16 * 1024, (), FLASH_PADDED),
    # a 2 KiB-aligned .bss input: a margin of RAM // 256 (64 B here) overflowed the RAM
    'flash_aligned': ('flash.ld', 16 * 1024, ('ALIGNED',), FLASH_PADDED),
    'ram_only': ('ram_only.ld', 32 * 1024, (), RAM_ONLY_PADDED),
    'ram_only_aligned': ('ram_only.ld', 32 * 1024, ('ALIGNED',), RAM_ONLY_PADDED),
    'ram_only_orphan': ('ram_only_orphan.ld', 32 * 1024, ('BOOT2',), RAM_ONLY_PADDED),
    # the same script for an image without the section: it takes no room
    'ram_only_orphan_absent': ('ram_only_orphan.ld', 32 * 1024, (), RAM_ONLY_PADDED),
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
        else:   # a skip without either; a failure if KVASIR_TEST_REQUIRE names arm-gcc
            armtc.need(None, 'arm-none-eabi-gcc')
            assembler = ['arm-none-eabi-gcc',
                         '-mcpu=cortex-m3', '-mthumb', '-c']
        for defines in {layout[2] for layout in LAYOUTS.values()}:
            obj = cls.tmp / f'image{"".join("_" + d for d in defines)}.o'
            subprocess.run(assembler + [f'-D{d}' for d in defines] +
                           [str(HERE / 'image.S'), '-o', str(obj)], check=True)
            cls.objects[defines] = obj

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def link(self, linker, layout, min_stack, heap):
        armtc.need(self, LINKERS[linker][0])
        script, ram, defines, _ = LAYOUTS[layout]
        elf = self.tmp / f'{linker}_{layout}_{min_stack}_{heap}.elf'
        cmd = LINKERS[linker] + [
            f'--library-path={LINKER_DIR}',
            f'--defsym=cmake_min_stack_size={min_stack}',
            f'--defsym=cmake_heap_size={heap}',
            '--defsym=cmake_core1_stack_size=0',
            '--defsym=cmake_core1_stack_in_scratch=0',
            '--defsym=cmake_scratch_banks=0',
            f'--script={HERE / script}', '--entry=ResetISR',
            str(self.objects[defines]), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result, elf, ram

    def sections(self, elf):
        """name -> (size, address) from the linked image"""
        tool = armtc.first(self, 'llvm-size', 'arm-none-eabi-size')
        out = subprocess.run([tool, '-A', '-d', str(elf)],
                             capture_output=True, text=True, check=True).stdout
        found = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[0].startswith('.') and parts[1].isdigit():
                found[parts[0]] = (int(parts[1]), int(parts[2]))
        return found

    def alignments(self, elf):
        """name -> alignment (the section header's Al) from the linked image"""
        tool = armtc.first(self, 'llvm-readelf', 'arm-none-eabi-readelf')
        out = subprocess.run([tool, '-S', '-W', str(elf)],
                             capture_output=True, text=True, check=True).stdout
        found = {}
        for line in out.splitlines():
            if ']' in line and line.lstrip().startswith('['):
                fields = line.split(']', 1)[1].split()
                if fields and fields[0].startswith('.') and fields[-1].isdigit():
                    found[fields[0]] = int(fields[-1])
        return found

    def margin(self, elf, layout):
        """the script's margin, from the image's own section alignments"""
        _, ram, _, padded = LAYOUTS[layout]
        aligns = self.alignments(elf)
        return max(sum(max(aligns.get(name, 1), 1) for name in padded) + 24, ram // 256)

    def symbols(self, elf):
        tool = armtc.first(self, 'llvm-nm', 'arm-none-eabi-nm')
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
        margin = self.margin(elf, layout)
        extra = ram - (others + min_stack + heap + margin)
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
        self.assertLess(ram_end - top, margin + 64)

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
        # everything fits in RAM, the margin does not: refused
        for linker in LINKERS:
            for layout in LAYOUTS:
                with self.subTest(linker=linker, layout=layout):
                    result, elf, ram = self.link(linker, layout, 1024, 0)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    secs = self.sections(elf)
                    others = sum(size for name, (size, addr) in secs.items()
                                 if 0x20000000 <= addr < 0x20000000 + ram and name not in ('.stack', '.heap'))
                    heap = ram - others - 1024 - self.margin(elf, layout) + 16
                    self.expect_refused(linker, layout, 1024, heap)


if __name__ == '__main__':
    unittest.main()
