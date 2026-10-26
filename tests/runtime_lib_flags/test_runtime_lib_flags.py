#!/usr/bin/env python3
"""Builds the SDK's libc++ sources with each ARM toolchain found (arm_gcc, arm_clang) on each C library (newlib, llvm),
through the real toolchain files and libcxx.cmake (CMakeLists.txt here), and fails on any warning or note.

A flag one compiler does not know is silent until a file warns; then gcc lists every unknown -Wno-* as a note. On a
gcc that reports `#pragma clang attribute` from libc++'s namespace macros (gcc 16 does not, other gccs did) every
libc++ file warned that way, with seven such notes each. A firmware build does not fail on it, so this test keeps
libcxx.cmake's flag lists right for both compilers. Two probes make it independent of the gcc at hand, compiled with
the flags a libc++ source gets: probe_pragma.cpp (the pragma region, outside a system header) must stay silent, and
probe_warning.cpp must give its one warning and no note about an unknown option.

    python3 tests/runtime_lib_flags/test_runtime_lib_flags.py -v
"""

import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / 'tests' / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

# the programs each toolchain file needs to configure; arm_clang asks arm-none-eabi-gcc for newlib (arm_clang.cmake)
TOOLCHAINS = {
    ('arm_gcc', 'newlib'): ['arm-none-eabi-g++', 'arm-none-eabi-gcc', 'arm-none-eabi-ld'],
    ('arm_gcc', 'llvm'): ['arm-none-eabi-g++', 'arm-none-eabi-gcc', 'arm-none-eabi-ld'],
    ('arm_clang', 'newlib'): ['clang++', 'arm-none-eabi-gcc'],
    ('arm_clang', 'llvm'): ['clang++'],
}
# a diagnostic in the build output: "warning:", "note:", gcc's "cc1plus: note: unrecognized command-line option ..."
DIAGNOSTIC = re.compile(
    r'\b(warning|note|error)\b:|unrecognized command-line option|unknown warning option')


class RuntimeLibFlags(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, toolchain, clib):
        armtc.need(self, *TOOLCHAINS[toolchain, clib])
        tree = Path(self.tmp.name) / f'{toolchain}_{clib}'
        generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
        configure = subprocess.run(['cmake', '-S', str(HERE), '-B', str(tree), *generator,
                                    f'-DCOMPILE_TARGET={toolchain}', f'-DCLIB={clib}'],
                                   capture_output=True, text=True)
        self.assertEqual(configure.returncode, 0,
                         configure.stdout + configure.stderr)
        # -j: the jobs of one tree; the sources are few, the four flag sets each compile them all
        built = subprocess.run(
            ['cmake', '--build', str(tree), '-j'], capture_output=True, text=True)
        output = built.stdout + built.stderr
        self.assertEqual(built.returncode, 0, output)
        noise = [line for line in output.splitlines() if DIAGNOSTIC.search(line)]
        self.assertEqual(
            noise, [], f'{toolchain} + {clib}: libc++ built with diagnostics:\n{output}')

        compiler, flags = (tree / 'libcxx_flags.txt').read_text().splitlines()
        flags = shlex.split(flags)

        def probe(name):
            result = subprocess.run([compiler, *flags, '-c', str(HERE / name), '-o', str(tree / f'{name}.o')],
                                    capture_output=True, text=True)
            return result.returncode, result.stdout + result.stderr

        rc, output = probe('probe_pragma.cpp')
        self.assertEqual((rc, output), (0, ''),
                         f'{toolchain} + {clib}: libc++\'s pragma region is not silent')
        rc, output = probe('probe_warning.cpp')
        self.assertEqual(rc, 0, output)
        self.assertIn('runtime_lib_flags probe', output)
        unknown = [line for line in output.splitlines()
                   if re.search(r'unrecognized command-line option|unknown warning option', line)]
        self.assertEqual(unknown, [
        ], f'{toolchain} + {clib}: libcxx.cmake passes options this compiler does not know')

    def test_gcc_newlib(self):
        self.build('arm_gcc', 'newlib')

    def test_gcc_llvm_libc(self):
        self.build('arm_gcc', 'llvm')

    def test_clang_newlib(self):
        self.build('arm_clang', 'newlib')

    def test_clang_llvm_libc(self):
        self.build('arm_clang', 'llvm')


if __name__ == '__main__':
    unittest.main()
