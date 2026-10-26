#!/usr/bin/env python3
"""Builds ram_funcs.cpp for a Cortex-M33 with every ARM toolchain found (arm-none-eabi-g++ and
clang, both with LTO as the firmwares link) and holds cmake/tools/check_ram_funcs.py to it: a
correct image passes, each broken one fails for its reason.

    python3 tests/ram_funcs/test_check_ram_funcs.py -v [--core-common <core_cortex_common checkout>]

CASE 11 and 12 include core_cortex_common's InterruptMask.hpp / Delay.hpp: the directory from --core-common (tests/CMakeLists.txt passes
the sibling checkout or a fetched one), else the sibling ../core_cortex_common, else that case is skipped.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
CHECK = SDK / 'cmake' / 'tools' / 'check_ram_funcs.py'
sys.path.insert(0, str(SDK / 'tests' / 'tooling'))
import armtc  # noqa: E402  the toolchain table, and when a missing tool is a skip or a failure


def take_core_common():
    """--core-common <dir> out of sys.argv (unittest must not see it), or the sibling checkout, or None."""
    if '--core-common' in sys.argv:
        i = sys.argv.index('--core-common')
        path = Path(sys.argv[i + 1])
        del sys.argv[i:i + 2]
        return path
    sibling = SDK.parent / 'core_cortex_common'
    return sibling if (sibling / 'InterruptMask.hpp').exists() else None


CORE_COMMON = take_core_common()

COMMON = ['-mcpu=cortex-m33', '-mthumb', '-std=c++26', '-Os', '-g', '-flto', '-ffunction-sections',
          '-fdata-sections', '-nostdlib', '-Wall', '-Wextra', '-Werror', f'-I{SDK / "src"}',
          f'-T{HERE / "test.ld"}', '-Wl,--gc-sections']
# flags, and the programs the build needs
TOOLCHAINS = armtc.TOOLCHAINS
# inlining left to always_inline alone: what a RAM function may rely on. gcc keeps -fno-inline through LTO; clang's
# LTO link runs its own inliner, so clang compiles without LTO here
NO_INLINE = {
    'gcc': ['-fno-inline'],
    'clang': ['-fno-inline', '-fno-lto'],
}


def std_headers(toolchain):
    """The C++ library headers for a case that includes them: arm-none-eabi-g++ brings libstdc++; clang gets the
    SDK's libc++ and llvm-libc (lib/, submodules) as cmake/arm_clang.cmake gives them, with the two settings
    arm_compiler_common.cmake defines instead of a __config_site."""
    if toolchain == 'gcc':
        return []
    resource = subprocess.run(['clang++', '-print-resource-dir'], capture_output=True, text=True,
                              check=True).stdout.strip()
    lib = SDK / 'lib'
    return ['-nostdinc'] + [f'-isystem{d}' for d in (lib / 'libcxx' / 'include', lib / 'libcxx' / 'src',
                                                     lib / 'libc' / 'include', lib / 'libc',
                                                     Path(resource) / 'include')] + \
        ['-D_LIBCPP_HARDENING_MODE_DEFAULT=_LIBCPP_HARDENING_MODE_FAST',
         '-D_LIBCPP_ASSERTION_SEMANTIC_DEFAULT=_LIBCPP_ASSERTION_SEMANTIC_HARDENING_DEPENDENT']


class CheckRamFuncs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, toolchain, case, *defines, extra=()):
        flags, exes = TOOLCHAINS[toolchain]
        armtc.need(self, *exes)
        elf = Path(self.tmp.name) / \
            f'{toolchain}_{case}_{"_".join(defines) or "plain"}_{len(extra)}.elf'
        cmd = flags + COMMON + list(extra) + [f'-DCASE={case}'] + [f'-D{d}' for d in defines] + \
            [str(HERE / 'ram_funcs.cpp'), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def check(self, elf):
        result = subprocess.run([sys.executable, str(
            CHECK), str(elf)], capture_output=True, text=True)
        return result.returncode, result.stderr

    def expect_pass(self, toolchain, case=0, *defines, extra=()):
        rc, err = self.check(self.build(
            toolchain, case, *defines, extra=extra))
        self.assertEqual(rc, 0, err)

    def expect_fail(self, toolchain, case, *messages, defines=(), extra=()):
        rc, err = self.check(self.build(
            toolchain, case, *defines, extra=extra))
        self.assertEqual(
            rc, 1, f'the check passed an image it should refuse:\n{err}')
        for m in messages:
            self.assertIn(m, err)

    def each(self, fn):
        """fn(toolchain) once per ARM toolchain found here (armtc.toolchains: a missing required one fails), each a
        subTest"""
        for toolchain in armtc.toolchains(self):
            with self.subTest(toolchain):
                fn(toolchain)

    # every kind of RAM function lands in RAM, the marked flash call is let through
    def test_correct_image(self):
        self.each(lambda tc: self.expect_pass(tc))

    # the old bug: gcc + LTO dropped the section of member, template and inline functions
    def test_old_gcc_attributes_leave_functions_in_flash(self):
        self.expect_fail('gcc', 0, 'Member::run', 'not in RAM',
                         defines=['OLD_GCC_ATTRIBUTES'])

    def test_direct_flash_call(self):
        self.each(lambda tc: self.expect_fail(
            tc, 2, 'caller(int)', 'reaches flash', 'inFlash(int)'))

    def test_flash_call_two_levels_down(self):
        self.each(lambda tc: self.expect_fail(
            tc, 3, 'helper(int)', 'reaches flash', 'inFlash(int)'))

    # constant data in flash
    def test_flash_data_read(self):
        self.each(lambda tc: self.expect_fail(
            tc, 5, 'readsFlashTable(int)', 'reads flash', 'table'))

    # a RAM function calling into a per-core scratch bank (RP2040 SRAM4/5, outside `ram`): the
    # bank is RAM, followed, and a flash call behind it reported
    def test_flash_call_behind_scratch_bank(self):
        self.each(lambda tc: self.expect_fail(tc, 6, 'callsScratch(int)', 'inScratch(int)',
                                              'reaches flash', 'inFlash(int)'))

    def test_scratch_bank_callee(self):
        self.each(lambda tc: self.expect_pass(tc, 7))

    # the register field API in a RAM function: with inlining off nothing of it stays a call (FieldTuple once held a
    # std::array whose operator[] -Oz + UBSan left in flash; check_ram_funcs refused a firmware image)
    def no_inline(self, toolchain):
        # the skip build() would give, before std_headers asks the missing clang++ for its resource dir
        armtc.need(self, *TOOLCHAINS[toolchain][1])
        return NO_INLINE[toolchain] + std_headers(toolchain)

    def test_register_access_in_ram_function(self):
        self.each(lambda tc: self.expect_pass(tc, 8, extra=self.no_inline(tc)))

    # Kvasir::Memory (load, store, loadWords) in a RAM function, the same mode as CASE 8
    def test_memory_access_in_ram_function(self):
        self.each(lambda tc: self.expect_pass(
            tc, 10, extra=self.no_inline(tc)))

    # the core's PRIMASK API (Core::IrqGuard, IrqState) and barriers in a RAM function, the same mode as CASE 8: a
    # member or constructor without always_inline fails here deterministically, not by an inliner's whim
    def test_irq_mask_in_ram_function(self):
        if CORE_COMMON is None or not (CORE_COMMON / 'InterruptMask.hpp').exists():
            self.skipTest('no core_cortex_common checkout (--core-common)')
        self.each(lambda tc: self.expect_pass(
            tc, 11, extra=self.no_inline(tc) + [f'-I{CORE_COMMON}']))

    # the core's counted waits (Core::spinCycles, both forms) in a RAM function, the same mode as CASE 8
    def test_delay_in_ram_function(self):
        if CORE_COMMON is None or not (CORE_COMMON / 'Delay.hpp').exists():
            self.skipTest('no core_cortex_common checkout (--core-common)')
        self.each(lambda tc: self.expect_pass(
            tc, 12, extra=self.no_inline(tc) + [f'-I{CORE_COMMON}']))

    # the control: the same mode leaves std::array's operator[] a flash call, so CASE 8 would see the old bug
    def test_no_inline_mode_shows_std_array_call(self):
        self.each(lambda tc: self.expect_fail(tc, 9, 'stdArrayIndex(int)', 'reaches flash', 'operator[]',
                                              extra=self.no_inline(tc)))

    # the check reads the source list with llvm-dwarfdump and only notes its absence
    def test_unmarked_ram_function(self):
        armtc.need(self, 'llvm-dwarfdump')
        self.each(lambda tc: self.expect_fail(tc, 4, 'unmarked.hpp:',
                  'unmarked', 'without KVASIR_RAM_FUNC_MARK()'))


if __name__ == '__main__':
    unittest.main()
