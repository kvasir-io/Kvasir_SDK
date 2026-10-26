#!/usr/bin/env python3
"""cmake/tools/find_undefined_refs.py, the post-link undefined-symbol check: host objects compiled here without PIC
(the script reads any ELF32 file's symbol table through kvasir_elf; a GOT reference would be undefined itself), 32-bit
(-m32) so the reader takes them. Clean: exit 0, the file kept. An undefined symbol: exit 1, its name on stderr, the
file removed with --delete-on-failure and kept without. A weak undefined one: exit 0. A non-ELF file, a file without a
symbol table, a missing file: exit 2 (removed with the flag), never a pass. No cc that makes 32-bit objects is a skip,
and a failure when KVASIR_TEST_REQUIRE names the group of the ARM compiler that is missing (tests/tooling/armtc.py).

    python3 tests/undefined_refs/test_find_undefined_refs.py -v
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

SCRIPT = HERE.parent.parent / 'cmake' / 'tools' / 'find_undefined_refs.py'
SOURCES = {
    'clean': 'int f(void) { return 0; }\n',
    'undefined': 'extern int x; int f(void) { return x; }\n',
    'weak': 'extern int y __attribute__((weak)); int g(void) { return &y != 0; }\n',
}
# an ELF32 object: clang for a 32-bit ARM target needs no multilib, gcc -m32 does
COMPILERS = [cmd for cmd in (['clang', '--target=armv7m-none-eabi'], ['arm-none-eabi-gcc', '-mthumb'], ['cc', '-m32'])
             if shutil.which(cmd[0])]


def compile_all(directory):
    for cmd in COMPILERS:
        ok = True
        for (name, source) in SOURCES.items():
            (directory / f'{name}.c').write_text(source)
            result = subprocess.run(cmd + ['-c', '-O1', '-fno-pic', str(directory / f'{name}.c'), '-o',
                                           str(directory / f'{name}.o')], capture_output=True)
            ok = ok and result.returncode == 0
        if ok:
            return True
    return False


class FindUndefinedRefs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls.tmp.name)
        cls.ok = compile_all(cls.dir)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        if not self.ok:
            # none found: a skip, or a failure for a required group; one found that made no object: a failure
            found = armtc.first(self, 'clang', 'arm-none-eabi-gcc')
            self.fail(f'{found} made no ELF32 objects')

    def copy(self, name, tag):
        """A fresh copy of an object, so a delete in one case does not reach the next."""
        out = self.dir / f'{name}_{tag}.o'
        shutil.copy(self.dir / f'{name}.o', out)
        return out

    def run_script(self, elf, *args):
        return subprocess.run([sys.executable, '-B', str(SCRIPT), str(elf), *args], capture_output=True, text=True)

    def test_symbols(self):
        clean = self.copy('clean', 'x')
        result = self.run_script(clean, '--delete-on-failure')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(clean.exists())

        weak = self.copy('weak', 'x')
        result = self.run_script(weak, '--delete-on-failure')
        self.assertEqual(result.returncode, 0, result.stderr)

        kept = self.copy('undefined', 'kept')
        result = self.run_script(kept)
        self.assertEqual(result.returncode, 1)
        self.assertIn(f'{kept}: undefined: x\n', result.stderr)
        self.assertTrue(kept.exists())

        gone = self.copy('undefined', 'gone')
        result = self.run_script(gone, '--delete-on-failure')
        self.assertEqual(result.returncode, 1)
        self.assertIn('undefined: x\n', result.stderr)
        self.assertFalse(gone.exists())

    def test_not_an_elf_is_no_pass(self):
        not_elf = self.dir / 'not_an_elf.o'
        not_elf.write_text('not an ELF file\n')
        result = self.run_script(not_elf, '--delete-on-failure')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(not_elf.exists())

    def test_no_symbol_table_is_no_pass(self):
        objcopy = armtc.first(self, 'llvm-objcopy', 'arm-none-eabi-objcopy')
        no_symtab = self.copy('clean', 'stripped')
        subprocess.run([objcopy, '--strip-all', str(no_symtab)], check=True)
        result = self.run_script(no_symtab, '--delete-on-failure')
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(no_symtab.exists())

    def test_a_missing_file(self):
        result = self.run_script(self.dir / 'missing.elf')
        self.assertEqual(result.returncode, 2)
        self.assertIn('no such file', result.stderr)


if __name__ == '__main__':
    unittest.main()
