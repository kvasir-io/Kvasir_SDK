#!/usr/bin/env python3
"""cmake/tools/kvasir_elf.py, the one ELF reader of the post-link tools: links tests/elf_image/image.S with every ARM
toolchain found (clang + ld.lld, arm-none-eabi-gcc + GNU ld) into a flash and a RAM image and holds the sections,
program headers and symbols to `llvm-readelf --elf-output-style=JSON` as the oracle; the address lookups on their own
contracts (NOBITS, PROGBITS only, VMA vs load address); a relocatable object reads, only its load view raises; a
non-ELF32 file raises ValueError; elf_image.Elf is the same class (kvasir_bench imports it under that name). A missing
toolchain is a skip.

    python3 tests/kvasir_elf/test_kvasir_elf.py -v
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
sys.path.insert(0, str(SDK / 'cmake' / 'tools'))
sys.path.insert(0, str(SDK / 'tests' / 'tooling'))
import armtc  # noqa: E402
import elf_image  # noqa: E402
import kvasir_elf  # noqa: E402
from kvasir_elf import Elf  # noqa: E402

IMAGE = SDK / 'tests' / 'elf_image'
TOOLCHAINS = {
    'llvm': (['clang', '--target=armv6m-none-eabi', '-c'], ['ld.lld']),
    'gnu': (['arm-none-eabi-gcc', '-mcpu=cortex-m0plus', '-mthumb', '-c'], ['arm-none-eabi-ld']),
}


def name(symbol):
    """The symbol's own name: readelf prints a section symbol (STT_SECTION, st_name 0) with its section's name."""
    return '' if symbol['Type']['Value'] == 3 else symbol['Name']['Name']


class KvasirElf(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_ok(self, cmd):
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f'{" ".join(map(str, cmd))}\n{result.stderr}')
        return result.stdout

    def obj(self, toolchain):
        assembler, _linker = TOOLCHAINS[toolchain]
        armtc.need(self, assembler[0])
        obj = Path(self.tmp.name) / f'{toolchain}.o'
        if not obj.exists():
            self.run_ok(assembler + [str(IMAGE / 'image.S'), '-o', str(obj)])
        return obj

    def link(self, toolchain, script):
        _assembler, linker = TOOLCHAINS[toolchain]
        armtc.need(self, linker[0])
        tmp = Path(self.tmp.name)
        ld, elf = tmp / f'{toolchain}_{script}', tmp / \
            f'{toolchain}_{script}.elf'
        ld.write_text(
            (IMAGE / script).read_text().replace('FLASH_ORIGIN', '0x10000000'))
        self.run_ok(linker + ['-T', str(ld), '--gc-sections',
                    str(self.obj(toolchain)), '-o', str(elf)])
        return elf

    def oracle(self, path):
        armtc.need(self, 'llvm-readelf')
        return json.loads(self.run_ok(['llvm-readelf', '--elf-output-style=JSON', '-S', '-s', '-l', str(path)]))[0]

    def check_against_readelf(self, path):
        elf = Elf.from_path(path)
        want = self.oracle(path)
        sections = [s['Section'] for s in want['Sections']]
        self.assertEqual([(s['name'], s['type'], s['flags'], s['addr'], s['offset'], s['size'], s['link'], s['align'],
                           s['entsize']) for s in elf.sections],
                         [(s['Name']['Name'], s['Type']['Value'], s['Flags']['Value'], s['Address'], s['Offset'],
                           s['Size'], s['Link'], s['AddressAlignment'], s['EntrySize']) for s in sections])
        segments = [p['ProgramHeader'] for p in want.get('ProgramHeaders', [])]
        self.assertEqual(elf.segments, [(p['Type']['Value'], p['Offset'], p['VirtualAddress'], p['PhysicalAddress'],
                                         p['FileSize'], p['MemSize'], p['Flags']['Value'], p['Alignment'])
                                        for p in segments])
        symbols = [s['Symbol'] for s in want['Symbols']]
        self.assertEqual([(s['name'], s['value'], s['size'], s['type'], s['bind'], s['shndx']) for s in elf.symbols],
                         [(name(s), s['Value'], s['Size'], s['Type']['Value'], s['Binding']['Value'],
                           s['Section']['Value']) for s in symbols])
        for s in symbols:
            if name(s):
                first = next(x for x in symbols if name(x) == name(s))
                self.assertEqual(elf.symbol(name(s)),
                                 (first['Value'], first['Size']))
        return elf

    def test_images(self):
        for toolchain in TOOLCHAINS:
            for script in ('flash.ld', 'ram.ld'):
                with self.subTest(toolchain=toolchain, script=script):
                    elf = self.check_against_readelf(
                        self.link(toolchain, script))
                    self.assertIsNone(elf.symbol('no_such_symbol'))
                    pieces = elf.loaded()
                    self.assertTrue(pieces)
                    self.assertEqual([lma for (_n, lma, _d) in pieces], sorted(
                        lma for (_n, lma, _d) in pieces))
                    self.assertEqual(elf.load_addresses(), {
                                     n: lma for (n, lma, _d) in pieces})
                    for sec in elf.alloc_sections():
                        if not sec['size']:
                            continue
                        self.assertIs(elf.alloc_section_at(sec['addr']), sec)
                        got = elf.read(sec['addr'], sec['size'])
                        if sec['type'] == kvasir_elf.SHT_NOBITS:
                            self.assertIsNone(got)
                            self.assertIsNone(elf.bytes_at(sec['addr'], 1))
                        else:
                            body = elf.data[sec['offset']:sec['offset'] + sec['size']]
                            self.assertEqual(got, body)
                            self.assertEqual(elf.bytes_at(
                                sec['addr'], sec['size']), body)
                            self.assertIsNone(
                                elf.read(sec['addr'], sec['size'] + 1))
                        if sec['type'] == kvasir_elf.SHT_PROGBITS:
                            self.assertIsNotNone(
                                elf.progbits_at(sec['addr'], sec['size']))
                    for (name, lma, data) in pieces:
                        sec = elf.section(name)
                        self.assertIs(elf.section_of(sec['addr']), sec)
                        self.assertEqual(elf.load_address(sec['addr']), lma)
                    for (name, addr, data) in elf.resident():
                        sec = elf.section(name)
                        self.assertEqual(sec['addr'], addr)
                        self.assertFalse(sec['flags'] & kvasir_elf.SHF_WRITE)

    def test_relocatable_object(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                elf = self.check_against_readelf(self.obj(toolchain))
                self.assertEqual(elf.segments, [])
                with self.assertRaisesRegex(ValueError, 'no program headers'):
                    elf.loaded()

    def test_not_an_elf32(self):
        for data in (b'not an elf at all', b'\x7fELF\x02\x01' + bytes(58), b'\x7fELF\x01\x02' + bytes(46)):
            with self.assertRaisesRegex(ValueError, 'not a little-endian ELF32 file'):
                Elf(data)

    def test_bytearray_kept(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                data = bytearray(self.link(toolchain, 'flash.ld').read_bytes())
                self.assertIs(Elf(data).data, data)

    def test_elf_image_reexports(self):
        self.assertIs(elf_image.Elf, Elf)

    def test_merge_contiguous(self):
        self.assertEqual(kvasir_elf.merge_contiguous([(8, b'\x03'), (0, b'\x01\x02'), (2, b''), (2, b'\x09')]),
                         [(0, b'\x01\x02\x09'), (8, b'\x03')])


if __name__ == '__main__':
    unittest.main()
