#!/usr/bin/env python3
"""Builds ram_funcs.cpp for a Cortex-M33 with every ARM toolchain found (arm-none-eabi-g++ and
clang, both with LTO as the firmwares link) and holds cmake/tools/check_ram_funcs.py to it: a
correct image passes, each broken one fails for its reason.

    python3 tests/ram_funcs/test_check_ram_funcs.py -v
"""

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
CHECK = SDK / 'cmake' / 'tools' / 'check_ram_funcs.py'

COMMON = ['-mcpu=cortex-m33', '-mthumb', '-std=c++26', '-Os', '-g', '-flto', '-ffunction-sections',
          '-fdata-sections', '-nostdlib', '-Wall', '-Wextra', '-Werror', f'-I{SDK / "src"}',
          f'-T{HERE / "test.ld"}', '-Wl,--gc-sections']
# flags, and the programs the build needs (clang without lld cannot link: the CI's plain clang job)
TOOLCHAINS = {
    'gcc': (['arm-none-eabi-g++', '-nostartfiles'], ['arm-none-eabi-g++']),
    'clang': (['clang++', '--target=armv8m.main-none-eabi', '-fuse-ld=lld'], ['clang++', 'ld.lld']),
}


class CheckRamFuncs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, toolchain, case, *defines):
        flags, exes = TOOLCHAINS[toolchain]
        for exe in exes:
            if shutil.which(exe) is None:
                self.skipTest(f'{exe} not found')
        elf = Path(self.tmp.name) / \
            f'{toolchain}_{case}_{"_".join(defines) or "plain"}.elf'
        cmd = flags + COMMON + [f'-DCASE={case}'] + [f'-D{d}' for d in defines] + \
            [str(HERE / 'ram_funcs.cpp'), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def check(self, elf):
        result = subprocess.run([sys.executable, str(
            CHECK), str(elf)], capture_output=True, text=True)
        return result.returncode, result.stderr

    def expect_pass(self, toolchain, case=0, *defines):
        rc, err = self.check(self.build(toolchain, case, *defines))
        self.assertEqual(rc, 0, err)

    def expect_fail(self, toolchain, case, *messages, defines=()):
        rc, err = self.check(self.build(toolchain, case, *defines))
        self.assertEqual(
            rc, 1, f'the check passed an image it should refuse:\n{err}')
        for m in messages:
            self.assertIn(m, err)

    # every kind of RAM function lands in RAM, the marked flash call is let through
    def test_correct_image_gcc(self):
        self.expect_pass('gcc')

    def test_correct_image_clang(self):
        self.expect_pass('clang')

    # the 2026-09-24 bug: gcc + LTO dropped the section of member, template and inline functions
    def test_old_gcc_attributes_leave_functions_in_flash(self):
        self.expect_fail('gcc', 0, 'Member::run', 'not in RAM',
                         defines=['OLD_GCC_ATTRIBUTES'])

    def test_direct_flash_call_gcc(self):
        self.expect_fail('gcc', 2, 'caller(int)',
                         'reaches flash', 'inFlash(int)')

    def test_direct_flash_call_clang(self):
        self.expect_fail('clang', 2, 'caller(int)',
                         'reaches flash', 'inFlash(int)')

    def test_flash_call_two_levels_down_gcc(self):
        self.expect_fail('gcc', 3, 'helper(int)',
                         'reaches flash', 'inFlash(int)')

    def test_flash_call_two_levels_down_clang(self):
        self.expect_fail('clang', 3, 'helper(int)',
                         'reaches flash', 'inFlash(int)')

    # the check reads the source list with llvm-dwarfdump and only notes its absence
    def expect_unmarked_fail(self, toolchain):
        if shutil.which('llvm-dwarfdump') is None:
            self.skipTest('llvm-dwarfdump not found')
        self.expect_fail(toolchain, 4, 'unmarked.hpp:', 'unmarked',
                         'without KVASIR_RAM_FUNC_MARK()')

    def test_unmarked_ram_function_gcc(self):
        self.expect_unmarked_fail('gcc')

    def test_unmarked_ram_function_clang(self):
        self.expect_unmarked_fail('clang')


if __name__ == '__main__':
    unittest.main()
