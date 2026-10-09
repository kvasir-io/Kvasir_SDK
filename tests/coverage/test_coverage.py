#!/usr/bin/env python3
"""Kvasir::Coverage (kvasir/Util/Coverage.hpp) and the bare-metal profile runtime (lib/compiler-rt/profile.cmake's
sources, COMPILER_RT_PROFILE_BAREMETAL) on the host: an instrumented lib.cpp, the driver that lets Coverage build its
manifest and assembles the .profraw from it - which must equal the runtime's own __llvm_profile_write_buffer byte for
byte - then llvm-profdata takes the file and llvm-cov reports `never` unrun and classify's negative branch uncovered.
Both counter modes (one byte a region, 64-bit counts). Without clang++ or the llvm tools it is a skip.

    python3 tests/coverage/test_coverage.py -v
"""

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
RT = SDK / 'lib' / 'compiler-rt'


def runtime_sources():
    text = (RT / 'profile.cmake').read_text()
    block = re.search(
        r'set\(COMPILER_RT_PROFILE_SOURCE_FILES\n(.*?)\n\)', text, re.S).group(1)
    return [RT / line.strip() for line in block.splitlines() if line.strip() and not line.strip().startswith('#')]


MODES = {
    'bytes': ['-mllvm', '-enable-single-byte-coverage=true'], 'counts': []}


class Coverage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, mode):
        for tool in ('clang', 'clang++', 'llvm-profdata', 'llvm-cov'):
            if shutil.which(tool) is None:
                self.skipTest(f'{tool} not found')
        out = Path(self.tmp.name) / mode
        out.mkdir()
        objects = []
        for source in runtime_sources():
            obj = out / (source.stem + '.o')
            subprocess.run(['clang', '-c', '-O1', '-DCOMPILER_RT_PROFILE_BAREMETAL=1', f'-I{RT / "profile"}',
                            f'-I{RT / "include"}', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        lib = out / 'lib.o'
        subprocess.run(['clang++', '-std=c++20', '-c', '-O1', '-fprofile-instr-generate', '-fcoverage-mapping',
                        *MODES[mode], str(HERE / 'lib.cpp'), '-o', str(lib)], check=True)
        exe = out / 'driver'
        # no -fprofile-instr-generate at the link: the driver must not bring the host's own profile runtime
        subprocess.run(['clang++', '-std=c++26', '-O1', '-DKVASIR_COVERAGE=1', f'-I{SDK / "src"}',
                        str(HERE / 'driver.cpp'), str(lib), *
                        objects, '-Wl,--gc-sections', '-Wl,--build-id=none',
                        '-o', str(exe)], check=True)
        profraw = out / 'manifest.profraw'
        run = subprocess.run([str(exe), str(profraw)],
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        profdata = out / 'manifest.profdata'
        subprocess.run(['llvm-profdata', 'merge', '-sparse',
                       str(profraw), '-o', str(profdata)], check=True)
        report = subprocess.run(['llvm-cov', 'export', '-summary-only', str(exe), f'-instr-profile={profdata}',
                                 str(HERE / 'lib.cpp')], capture_output=True, text=True, check=True)
        files = json.loads(report.stdout)['data'][0]['files']
        summary = next(f['summary']
                       for f in files if f['filename'].endswith('lib.cpp'))
        self.assertEqual(summary['functions']['count'], 2)
        # never() did not run
        self.assertEqual(summary['functions']['covered'], 1)
        self.assertLess(summary['regions']['covered'],
                        summary['regions']['count'])   # nor did x < 0

    def test_single_byte_counters(self):
        self.run_mode('bytes')

    def test_64_bit_counters(self):
        self.run_mode('counts')


if __name__ == '__main__':
    unittest.main()
