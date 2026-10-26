#!/usr/bin/env python3
"""cmake/tools/beautify_lst.py, the listing's name shortener (kvasir_postlink.py pipes objdump through it): the three
line shapes it rewrites past 120 characters (a function header, a symbol-table row, a long <name> inside a line), the
lines it leaves alone, the name rules it applies (a register apply, a namespace chain, a long mangled name), a long
mangled name without '::' in linear time (a gcc listing took minutes with the old quadratic scan), and the streaming
entry points (beautify on any line iterable, `-` as standard input). No toolchain needed.

    python3 tests/beautify_lst/test_beautify_lst.py -v
"""

import io
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent.parent / 'cmake' / 'tools'
sys.path.insert(0, str(TOOLS))
import beautify_lst  # noqa: E402

REGISTER_APPLY = ('void Kvasir::Register::apply<Kvasir::Register::Action<Kvasir::Register::FieldLocation<Kvasir::Register::'
                  'Address<1073905664u, 0u, 0u, unsigned int>, 4294967295u, Kvasir::Peripheral::SIO::Registers<>::'
                  'GPIO_OUT_SETVal, unsigned int>, Kvasir::Register::WriteLiteral<32u>>>()')
LONG_CHAIN = 'a::b::c::d::E<' + 'x' * 250 + '>::f()'


class ShortenLine(unittest.TestCase):
    def test_function_header(self):
        self.assertEqual(beautify_lst.shorten_line(f'10000100 <{REGISTER_APPLY}>:\n'),
                         '10000100 <[Kvasir::Register::apply<SIO>]>:\n')

    def test_symbol_table_row(self):
        self.assertEqual(beautify_lst.shorten_line(f'10000234 g     F .text\t00000010 {REGISTER_APPLY}\n'),
                         '10000234 g     F .text\t00000010 [Kvasir::Register::apply<SIO>]\n')

    def test_long_name_in_an_instruction(self):
        # an instruction row is taken by the symbol-row rule (its operand is the "name"): the target name shortened
        self.assertEqual(beautify_lst.shorten_line(f'10000300:  f000 f800  \tbl\t0x10000400 <{LONG_CHAIN}>\n'),
                         '10000300:  f000 f800  \tbl\t0x10000400 [a::...::d::E<...>]\n')

    def test_left_alone(self):
        for line in ('short line\n', '10000100 <main>:\n', '\n', 'x' * 120 + '\n'):
            with self.subTest(line=line[:20]):
                self.assertEqual(beautify_lst.shorten_line(line), line)

    def test_names(self):
        self.assertEqual(beautify_lst.extract_meaningful_name('Kvasir::Startup::Detail::Phase::run<x>'),
                         '[Kvasir::...::Phase::run<...>]')
        self.assertEqual(beautify_lst.extract_meaningful_name(
            'ns::f<int>'), '[ns::f<...>]')
        self.assertEqual(beautify_lst.extract_meaningful_name(
            '_Z' + 'a1' * 60), '_Z' + 'a1' * 37 + 'a...')
        self.assertEqual(
            beautify_lst.extract_meaningful_name('plain'), 'plain')

    def test_long_mangled_name_is_linear(self):
        line = '10000100 <_ZN' + 'a1' * 12000 + '>:\n'   # unguarded: ~1.5 s here
        start = time.perf_counter()
        out = beautify_lst.shorten_line(line)
        self.assertLess(time.perf_counter() - start, 0.3)
        self.assertTrue(out.endswith('...>:\n') and len(out) < 120, out)


class Streams(unittest.TestCase):
    def test_beautify_streams(self):
        out = io.StringIO()
        beautify_lst.beautify(
            iter(['a\n', f'10000100 <{REGISTER_APPLY}>:\n']), out)
        self.assertEqual(
            out.getvalue(), 'a\n10000100 <[Kvasir::Register::apply<SIO>]>:\n')

    def test_main_reads_standard_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'sub' / 'out.lst'
            result = subprocess.run([sys.executable, '-B', str(TOOLS / 'beautify_lst.py'), '-', str(target)],
                                    input=f'x\n10000100 <{REGISTER_APPLY}>:\n', capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_text(),
                             'x\n10000100 <[Kvasir::Register::apply<SIO>]>:\n')
            missing = subprocess.run([sys.executable, '-B', str(TOOLS / 'beautify_lst.py'), str(Path(tmp) / 'no.lst'),
                                      str(target)], capture_output=True, text=True)
            self.assertEqual(missing.returncode, 1)
            self.assertIn('Input file not found', missing.stderr)


if __name__ == '__main__':
    unittest.main()
