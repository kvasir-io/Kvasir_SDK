#!/usr/bin/env python3
"""cmake/tools/elf_image.py, the flash artefacts from the load view: links image.S with every ARM toolchain found
(clang + ld.lld, arm-none-eabi-gcc + GNU ld) into an RP-like flash image (flash.ld at 0x10000000), a SAM-like one
past 64 KiB at address 0 (hex type 02 records) and a RAM image (ram.ld), and holds every artefact to llvm-objcopy
with the matching --only-section list as the oracle: the section no list names (.trap_data) is in _flash.hex/.bin/
.uf2 at its load address, .eeprom only in the _eeprom* files, the NOLOAD buffer and the non-ALLOC section nowhere,
_flash.elf without an empty PT_LOAD. A missing toolchain is a skip.

    python3 tests/elf_image/test_elf_image.py -v
"""

import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOL = HERE.parent.parent / 'cmake' / 'tools' / 'elf_image.py'

spec = importlib.util.spec_from_file_location('elf_image', TOOL)
elf_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf_image)

TOOLCHAINS = {
    'llvm': (['clang', '--target=armv6m-none-eabi', '-c'], ['ld.lld']),
    'gnu': (['arm-none-eabi-gcc', '-mcpu=cortex-m0plus', '-mthumb', '-c'], ['arm-none-eabi-ld']),
}
FLASH = ['.vectors', '.text', '.data', '.trap_data']
FAMILY = 0xE48BFF59
TRAP_WORDS = struct.pack('<II', 0x7A5EC7ED, 0x01020304)
EEPROM_WORDS = struct.pack('<II', 0xEE00EE00, 0xEE11EE11)
META_WORD = struct.pack('<I', 0xBADBADBA)
NOLOAD_SIZE = 64
SUFFIXES = ['_flash.bin', '_flash.hex', '_flash.uf2', '_eeprom.bin', '_eeprom.hex', '_eeprom.uf2',
            '_eeprom_flash.hex', '_eeprom_flash.uf2']


def uf2_bytes(data):
    """address -> byte of a .uf2, every block checked for the page grid elf_image.py writes."""
    out = {}
    count = len(data) // 512
    for number in range(count):
        block = data[512 * number:512 * (number + 1)]
        (m0, m1, flags, address, size, index, total,
         family) = struct.unpack_from('<IIIIIIII', block)
        assert (m0, m1, flags, size, index, total, family) == (0x0A324655, 0x9E5D5157, 0x2000, 256, number, count,
                                                               FAMILY), block[:32]
        assert address % 256 == 0 and struct.unpack_from('<I', block, 508)[
            0] == 0x0AB16F30
        for i, byte in enumerate(block[32:32 + 256]):
            out[address + i] = byte
    return out


class ElfImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.objcopy = shutil.which('llvm-objcopy')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def link(self, toolchain, script, origin=0x10000000, big=False):
        assembler, linker = TOOLCHAINS[toolchain]
        for exe in [assembler[0], linker[0], 'llvm-objcopy']:
            if shutil.which(exe) is None:
                self.skipTest(f'{exe} not found')
        name = f'{toolchain}_{Path(script).stem}_{origin:x}{"_big" if big else ""}'
        tmp = Path(self.tmp.name)
        obj, elf, ld = tmp / f'{name}.o', tmp / \
            f'{name}.elf', tmp / f'{name}.ld'
        ld.write_text(
            (HERE / script).read_text().replace('FLASH_ORIGIN', hex(origin)))
        for cmd in (assembler + (['-DBIG'] if big else []) + [str(HERE / 'image.S'), '-o', str(obj)],
                    linker + ['-T', str(ld), '--gc-sections', str(obj), '-o', str(elf)]):
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0,
                             f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def images(self, elf):
        prefix = elf.with_suffix('')
        result = subprocess.run([sys.executable, '-B', str(TOOL), str(elf), '--out-prefix', str(prefix),
                                 '--uf2-family', hex(FAMILY), '--objcopy', self.objcopy], capture_output=True,
                                text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return {s: Path(f'{prefix}{s}').read_bytes() for s in SUFFIXES + ['_flash.elf']}

    def oracle(self, elf, sections, target):
        out = elf.with_name(elf.stem + '_oracle_' + '_'.join(s.strip('.')
                            for s in sections) + '.' + target)
        subprocess.run([self.objcopy, '-O', target, *[f'--only-section={s}' for s in sections], str(elf), str(out)],
                       check=True)
        return out.read_bytes()

    def check(self, elf, eeprom=True):
        got = self.images(elf)
        flash_all = FLASH + (['.eeprom'] if eeprom else [])
        self.assertEqual(got['_flash.hex'], self.oracle(elf, FLASH, 'ihex'))
        self.assertEqual(got['_flash.bin'], self.oracle(elf, FLASH, 'binary'))
        self.assertEqual(got['_eeprom.hex'],
                         self.oracle(elf, ['.eeprom'], 'ihex'))
        self.assertEqual(got['_eeprom.bin'], self.oracle(
            elf, ['.eeprom'], 'binary'))
        self.assertEqual(got['_eeprom_flash.hex'],
                         self.oracle(elf, flash_all, 'ihex'))
        # the trap section is flashed, the metadata and the EEPROM bytes are not in the flash image
        self.assertIn(TRAP_WORDS, got['_flash.bin'])
        for suffix in SUFFIXES:
            self.assertNotIn(META_WORD, got[suffix], suffix)
        self.assertNotIn(EEPROM_WORDS, got['_flash.bin'])
        self.assertEqual(EEPROM_WORDS in got['_eeprom.bin'], eeprom)
        # the uf2 carries exactly the bin's bytes at the bin's addresses (padding 0x00 to the page grid)
        pieces = elf_image.Elf(elf.read_bytes()).loaded()
        flash = [p for p in pieces if p[0] != '.eeprom']
        start = flash[0][1]
        uf2 = uf2_bytes(got['_flash.uf2'])
        for offset, byte in enumerate(got['_flash.bin']):
            self.assertEqual(uf2[start + offset], byte, hex(start + offset))
        self.assertTrue(all(v == 0 for a, v in uf2.items()
                        if not start <= a < start + len(got['_flash.bin'])))
        # _flash.elf: the selected sections, no PT_LOAD without one
        shrunk = elf_image.Elf(got['_flash.elf'])
        self.assertEqual(sorted(n for (n, _a, _d) in shrunk.loaded()), sorted(
            n for (n, _a, _d) in flash))
        for (p_type, _o, _v, _p, filesz, memsz, *_r) in shrunk.segments:
            if p_type == elf_image.PT_LOAD:
                self.assertTrue(filesz or memsz)
        return got

    def test_flash_image(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                self.check(self.link(toolchain, 'flash.ld'))

    def test_flash_at_zero_past_64k(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                got = self.check(
                    self.link(toolchain, 'flash.ld', origin=0, big=True))
                self.assertIn(b':020000021000EC', got['_flash.hex'])
                self.assertGreater(max(uf2_bytes(got['_flash.uf2'])), 0x10000)

    def test_ram_image(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                got = self.check(self.link(toolchain, 'ram.ld'), eeprom=False)
                self.assertEqual(min(uf2_bytes(got['_flash.uf2'])), 0x20000000)

    def test_refuses_what_is_not_an_image(self):
        bad = Path(self.tmp.name) / 'not_an_elf.elf'
        bad.write_bytes(b'\x7fELF\x02\x01' + bytes(58))
        result = subprocess.run(
            [sys.executable, '-B', str(TOOL), str(bad), '--list'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('not a little-endian ELF32 file', result.stderr)


if __name__ == '__main__':
    unittest.main()
