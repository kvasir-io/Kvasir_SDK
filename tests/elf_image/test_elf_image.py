#!/usr/bin/env python3
"""cmake/tools/elf_image.py, the flash artefacts from the load view: links image.S with every ARM toolchain found
(clang + ld.lld, arm-none-eabi-gcc + GNU ld) into an RP-like flash image (flash.ld at 0x10000000), a SAM-like one
past 64 KiB at address 0 (hex type 02 records) and a RAM image (ram.ld), and holds every artefact to llvm-objcopy
with the matching --only-section list as the oracle: the section no list names (.trap_data) is in _flash.hex/.bin/
.uf2 at its load address, .eeprom only in the _eeprom* files, the NOLOAD buffer and the non-ALLOC section nowhere,
_flash.elf without an empty PT_LOAD or one with a hole larger than the next section's alignment, split only there (the
RAM image's NOLOAD buffer sits between two loaded sections, as common_ram_only.ld's .noInitLowRam and .ramExec, and an
8-byte alignment hole picotool plugs itself between .vectors and .text). The image end check: the fixtures define
`_LINKER_image_end_flash_` as common.ld does (the flash image's real end, 0 in the RAM image); the same flash image with
it moved in front of .trap_data fails (exit 1, names .trap_data and its bytes, nothing written, the ELF gone with
--delete-on-failure), at the real end, past it or 0 it passes, and an image without it is malformed (exit 2). A missing
toolchain is a skip.

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
sys.path.insert(0, str(HERE.parent.parent / 'tests' / 'tooling'))
import armtc  # noqa: E402  when a missing tool is a skip or a failure

spec = importlib.util.spec_from_file_location('elf_image', TOOL)
elf_image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(elf_image)
# imported by elf_image from cmake/tools
kvasir_elf = sys.modules['kvasir_elf']

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


def contents(elf):
    """The sections with bytes in the file that are loaded, in file order."""
    return sorted((s for s in elf.sections if s['flags'] & kvasir_elf.SHF_ALLOC and s['type'] != kvasir_elf.SHT_NOBITS
                   and s['size']), key=lambda s: s['offset'])


def segment_of(elf, name):
    """The index of the PT_LOAD that holds section `name`'s file bytes, None when there is none."""
    for sec in elf.sections:
        if sec['name'] == name:
            for (i, (p_type, p_offset, _v, _p, filesz, *_r)) in enumerate(elf.segments):
                if p_type == kvasir_elf.PT_LOAD and p_offset <= sec['offset'] < p_offset + filesz:
                    return i
    return None


class ElfImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.objcopy = shutil.which('llvm-objcopy')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def link(self, toolchain, script, origin=0x10000000, big=False, image_end=None, no_image_end=False):
        """image_end: moves the end as a chip script does (_LINKER_INTERN_image_end_flash_); no_image_end: the script
        without the symbols."""
        assembler, linker = TOOLCHAINS[toolchain]
        armtc.need(self, assembler[0], linker[0], 'llvm-objcopy')
        name = f'{toolchain}_{Path(script).stem}_{origin:x}{"_big" if big else ""}'
        if image_end is not None:
            name += f'_end_{image_end:x}'
            linker = linker + \
                [f'--defsym=_LINKER_INTERN_image_end_flash_={image_end:#x}']
        text = (HERE / script).read_text().replace('FLASH_ORIGIN', hex(origin))
        if no_image_end:
            name += '_no_end'
            text = ''.join(line for line in text.splitlines(keepends=True)
                           if not (line.startswith(('_LINKER_', 'PROVIDE(_LINKER_')) and line.rstrip().endswith(';')))
        tmp = Path(self.tmp.name)
        obj, elf, ld = tmp / f'{name}.o', tmp / \
            f'{name}.elf', tmp / f'{name}.ld'
        ld.write_text(text)
        for cmd in (assembler + (['-DBIG'] if big else []) + [str(HERE / 'image.S'), '-o', str(obj)],
                    linker + ['-T', str(ld), '--gc-sections', str(obj), '-o', str(elf)]):
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0,
                             f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def run_tool(self, elf, *extra):
        return subprocess.run([sys.executable, '-B', str(TOOL), str(elf), '--out-prefix', str(elf.with_suffix('')),
                               '--uf2-family', hex(FAMILY), '--objcopy', self.objcopy, *extra], capture_output=True,
                              text=True)

    def images(self, elf):
        prefix = elf.with_suffix('')
        self.assertIsNotNone(kvasir_elf.Elf(
            elf.read_bytes()).symbol(elf_image.IMAGE_END))
        result = self.run_tool(elf)
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
        pieces = kvasir_elf.Elf(elf.read_bytes()).loaded()
        self.assertEqual(kvasir_elf.Elf(elf.read_bytes()).load_addresses(), {
                         n: a for (n, a, _d) in pieces})
        flash = [p for p in pieces if p[0] != '.eeprom']
        start = flash[0][1]
        uf2 = uf2_bytes(got['_flash.uf2'])
        for offset, byte in enumerate(got['_flash.bin']):
            self.assertEqual(uf2[start + offset], byte, hex(start + offset))
        self.assertTrue(all(v == 0 for a, v in uf2.items()
                        if not start <= a < start + len(got['_flash.bin'])))
        # _flash.elf: the selected sections, no PT_LOAD without one, none with a hole larger than the next section's
        # alignment (picotool seal: "Cannot plug gap greater than alignment"), and split only there: two sections
        # that shared a PT_LOAD across a smaller hole still do
        shrunk = kvasir_elf.Elf(got['_flash.elf'])
        self.assertEqual(sorted(n for (n, _a, _d) in shrunk.loaded()), sorted(
            n for (n, _a, _d) in flash))
        for (p_type, _o, _v, _p, filesz, memsz, *_r) in shrunk.segments:
            if p_type == kvasir_elf.PT_LOAD:
                self.assertTrue(filesz or memsz)
        for (first, following) in zip(contents(shrunk), contents(shrunk)[1:]):
            if segment_of(shrunk, first['name']) == segment_of(shrunk, following['name']):
                self.assertLessEqual(following['offset'] - first['offset'] - first['size'], following['align'],
                                     f'hole between {first["name"]} and {following["name"]}')
        original = kvasir_elf.Elf(elf.read_bytes())
        kept = [s for s in contents(original) if segment_of(
            shrunk, s['name']) is not None]
        for (first, following) in zip(kept, kept[1:]):
            if segment_of(original, first['name']) == segment_of(original, following['name']) \
                    and following['offset'] - first['offset'] - first['size'] <= following['align']:
                self.assertEqual(segment_of(shrunk, first['name']), segment_of(shrunk, following['name']),
                                 f'{first["name"]} and {following["name"]} split at a hole picotool plugs')
        # the bytes stay where they were: every section of _flash.elf loads at its address in the input
        self.assertEqual(sorted((n, a) for (n, a, _d) in shrunk.loaded()), sorted(
            (n, a) for (n, a, _d) in flash))
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

    def test_image_end(self):
        for toolchain in TOOLCHAINS:
            with self.subTest(toolchain=toolchain):
                plain = kvasir_elf.Elf(
                    self.link(toolchain, 'flash.ld').read_bytes())
                loaded = {n: (a, len(d)) for (n, a, d) in plain.loaded()}
                # where the image ends without the trap section
                data_end = loaded['.trap_data'][0]
                real_end = sum(loaded['.trap_data'])
                self.assertEqual(real_end - data_end, len(TRAP_WORDS))
                for (end, ok) in ((data_end, False), (real_end, True), (real_end + 4, True), (0, True)):
                    elf = self.link(toolchain, 'flash.ld', image_end=end)
                    self.assertEqual(kvasir_elf.Elf(
                        elf.read_bytes()).symbol(elf_image.IMAGE_END)[0], end)
                    for path in Path(self.tmp.name).glob(f'{elf.stem}_*'):
                        path.unlink()
                    result = self.run_tool(elf, '--delete-on-failure')
                    if ok:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertTrue(elf.exists())
                        continue
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(f'section .trap_data is loaded at {data_end:#010x}..{real_end:#010x}, '
                                  f'{len(TRAP_WORDS)} bytes past {elf_image.IMAGE_END} ({data_end:#010x})',
                                  result.stderr)
                    # .eeprom lies past the end too, and is not an error
                    self.assertNotIn('.eeprom', result.stderr)
                    self.assertFalse(elf.exists())
                    self.assertFalse(elf.with_name(
                        f'{elf.stem}_flash.bin').exists())

    def test_image_without_end_is_malformed(self):
        for toolchain in TOOLCHAINS:
            for script in ('flash.ld', 'ram.ld'):
                with self.subTest(toolchain=toolchain, script=script):
                    elf = self.link(toolchain, script, no_image_end=True)
                    self.assertIsNone(kvasir_elf.Elf(
                        elf.read_bytes()).symbol(elf_image.IMAGE_END))
                    result = self.run_tool(elf, '--delete-on-failure')
                    self.assertEqual(result.returncode, 2, result.stderr)
                    self.assertIn(
                        f'no {elf_image.IMAGE_END} symbol', result.stderr)
                    self.assertFalse(elf.exists())
                    self.assertFalse(elf.with_name(
                        f'{elf.stem}_flash.bin').exists())

    def test_refuses_what_is_not_an_image(self):
        bad = Path(self.tmp.name) / 'not_an_elf.elf'
        bad.write_bytes(b'\x7fELF\x02\x01' + bytes(58))
        result = subprocess.run(
            [sys.executable, '-B', str(TOOL), str(bad), '--list'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('not a little-endian ELF32 file', result.stderr)


if __name__ == '__main__':
    unittest.main()
