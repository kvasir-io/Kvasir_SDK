#!/usr/bin/env python3
"""cmake/tools/patch_image_crc.py, the IMAGE_CRC post-link step: builds image.cpp for a Cortex-M33 with every ARM
toolchain found (arm-none-eabi-g++ and clang, LTO as the firmwares link) and holds the patcher to it - the descriptor
names exactly the hex's records without its own 80 bytes, their zlib CRC, a second run changes nothing, the log
printer's parser (uc_log HexImage.hpp, when the nested uc_log and a host compiler are there) reads the same
segments; and it refuses an image without a descriptor, a descriptor outside the hex, a RAM segment, too many
segments. A RAM image (test_ram.ld) is checked where it runs: only its read-only sections - and .text also when a
static constructor's .init_array entry makes the linker call it writable -, --exclude cuts the picobin-like block
out, and it refuses a segment outside SRAM. A missing toolchain is a skip.

    python3 tests/image_crc/test_patch_image_crc.py -v
"""

import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent.parent
PATCH = SDK / 'cmake' / 'tools' / 'patch_image_crc.py'
UC_LOG_SRC = SDK / 'uc_log' / 'src'

spec = importlib.util.spec_from_file_location('patch_image_crc', PATCH)
pic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pic)

COMMON = ['-mcpu=cortex-m33', '-mthumb', '-std=c++26', '-Os', '-flto', '-ffunction-sections', '-fdata-sections',
          '-nostdlib', '-ffreestanding', '-nostdinc++', f'-I{HERE / "include"}', '-Wall', '-Wextra', '-Werror',
          f'-I{SDK / "src"}', '-Wl,--gc-sections']
TOOLCHAINS = {
    'gcc': (['arm-none-eabi-g++', '-nostartfiles'], ['arm-none-eabi-g++']),
    'clang': (['clang++', '--target=armv8m.main-none-eabi', '-fuse-ld=lld'], ['clang++', 'ld.lld']),
}
SECTIONS = ['.vectors', '.text', '.data', '.boot2']
EXCLUDE = ['--exclude', '_LINKER_INTERN_after_vectors_start_',
           '_LINKER_INTERN_after_vectors_end_']


def objcopy():
    for name in ('llvm-objcopy', 'arm-none-eabi-objcopy'):
        if shutil.which(name):
            return name
    return None


class PatchImageCrc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.objcopy = objcopy()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def build(self, toolchain, *defines, script='test.ld'):
        flags, exes = TOOLCHAINS[toolchain]
        for exe in exes + [self.objcopy]:
            if exe is None or shutil.which(exe) is None:
                self.skipTest(f'{exe or "an objcopy"} not found')
        elf = Path(self.tmp.name) / \
            f'{toolchain}_{Path(script).stem}_{"_".join(defines) or "plain"}.elf'
        cmd = flags + COMMON + [f'-T{HERE / script}'] + \
            [f'-D{d}' for d in defines] + \
            [str(HERE / 'image.cpp'), '-o', str(elf)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0,
                         f'{" ".join(cmd)}\n{result.stderr}')
        return elf

    def patch(self, elf, sections=SECTIONS, extra=()):
        names = ['--sections', *sections] if sections else []
        result = subprocess.run([sys.executable, '-B', str(PATCH), str(elf), '--objcopy', self.objcopy,
                                 *names, *extra], capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def hex_of(self, elf):
        return pic.flash_hex(elf, self.objcopy, SECTIONS)

    def descriptor(self, elf):
        e = pic.Elf(elf)
        address, _size = e.symbol(pic.SYMBOL)
        s = e.section_of(address)
        offset = s['offset'] + address - s['addr']
        words = struct.unpack_from('<20I', e.data, offset)
        return address, words

    def check_image(self, toolchain):
        elf = self.build(toolchain, 'KVASIR_IMAGE_CRC=1')
        address, words = self.descriptor(elf)
        self.assertEqual(words[0], pic.UNPATCHED)
        rc, out = self.patch(elf)
        self.assertEqual(rc, 0, out)
        address, words = self.descriptor(elf)
        magic, version, crc, count = words[:4]
        self.assertEqual((magic, version), (pic.MAGIC, pic.VERSION))
        segments = [(words[4 + 2 * i], words[5 + 2 * i]) for i in range(count)]
        self.assertTrue(all(w == 0 for w in words[4 + 2 * count:]))
        # the hex's records, the descriptor cut out: .boot2 alone (the gap behind it), .text in front of and behind
        # the descriptor... and .data right behind .text (one record run with it)
        hex_segments = pic.parse_intel_hex(self.hex_of(elf))
        expected = pic.cut(hex_segments, address, pic.DESCRIPTOR_BYTES)
        self.assertEqual(segments, [(a, len(d)) for a, d in expected])
        self.assertGreaterEqual(count, 2)
        self.assertEqual(segments[0][0], 0x10000000)
        self.assertNotIn(address, [a for a, _ in segments])
        want = 0
        for a, d in expected:
            want = zlib.crc32(d, want)
        self.assertEqual(crc, want)
        # the hex now carries the patched descriptor
        patched = b''.join(d for a, d in hex_segments)
        self.assertIn(struct.pack('<4I', pic.MAGIC,
                      pic.VERSION, crc, count), patched)
        self.assertEqual(out.strip(),
                         f'image crc: {count} segment(s), {sum(n for _, n in segments)} bytes, crc 0x{crc:08x}')
        # idempotent
        before = elf.read_bytes()
        rc, out = self.patch(elf)
        self.assertEqual(rc, 0, out)
        self.assertEqual(before, elf.read_bytes())
        return elf

    def test_gcc(self):
        self.check_image('gcc')

    def test_clang(self):
        self.check_image('clang')

    def test_printer_reads_the_same_segments(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if compiler is None or not (UC_LOG_SRC / 'uc_log' / 'detail' / 'HexImage.hpp').exists():
            self.skipTest('no host compiler or no nested uc_log')
        elf = self.check_image('clang' if shutil.which('ld.lld') else 'gcc')
        exe = Path(self.tmp.name) / 'crc_of_hex'
        result = subprocess.run([compiler, '-std=c++23', f'-I{UC_LOG_SRC}', str(HERE / 'crc_of_hex.cpp'), '-o',
                                 str(exe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        hex_path = Path(self.tmp.name) / 'printer.hex'
        hex_path.write_text(self.hex_of(elf))
        printer = subprocess.run(
            [str(exe), str(hex_path)], capture_output=True, text=True)
        self.assertEqual(printer.returncode, 0)
        crc, count = printer.stdout.split()
        segments = pic.parse_intel_hex(hex_path.read_text())
        self.assertEqual(int(count), len(segments))
        # the "build" number: descriptor included
        self.assertEqual(int(crc, 16), pic.image_crc(segments))

    def section(self, elf, name):
        e = pic.Elf(elf)
        s = next(s for s in e.sections if s['name'] == name)
        return s['addr'], bytes(e.data[s['offset']:s['offset'] + s['size']])

    def crc_in_elf(self, elf, segments):
        """The CRC over the segments, read from the ELF's loaded sections (as the firmware reads them in RAM)."""
        e = pic.Elf(elf)
        crc = 0
        for address, length in segments:
            s = e.section_of(address)
            offset = s['offset'] + address - s['addr']
            crc = zlib.crc32(bytes(e.data[offset:offset + length]), crc)
        return crc

    def poke(self, elf, address, value=0x5A):
        e = pic.Elf(elf)
        s = e.section_of(address)
        e.data[s['offset'] + address - s['addr']] ^= value
        elf.write_bytes(e.data)

    def check_ram_image(self, toolchain, *defines):
        elf = self.build(toolchain, 'KVASIR_IMAGE_CRC=1',
                         'RAM_IMAGE', *defines, script='test_ram.ld')
        rc, out = self.patch(elf, extra=EXCLUDE)
        self.assertEqual(rc, 0, out)
        self.assertTrue(out.strip().endswith(
            ', checked in RAM: .vectors .text (left out: .data)'), out)
        address, words = self.descriptor(elf)
        magic, _version, crc, count = words[:4]
        self.assertEqual(magic, pic.MAGIC)
        segments = [(words[4 + 2 * i], words[5 + 2 * i]) for i in range(count)]
        e = pic.Elf(elf)
        block_start = e.symbol('_LINKER_INTERN_after_vectors_start_')[0]
        block_end = e.symbol('_LINKER_INTERN_after_vectors_end_')[0]
        data_start, data = self.section(elf, '.data')
        text_start, text = self.section(elf, '.text')
        vectors_start, _ = self.section(elf, '.vectors')
        twice = e.symbol('twice')[0] & ~1

        def covered(a):
            return any(s <= a < s + n for s, n in segments)
        self.assertTrue(all(a >= pic.SRAM_BASE for a, _ in segments))
        self.assertTrue(covered(vectors_start) and covered(
            text_start) and covered(twice))
        self.assertFalse(any(covered(a)
                         for a in range(block_start, block_end)))
        self.assertFalse(any(covered(a)
                         for a in range(data_start, data_start + len(data))))
        self.assertFalse(any(covered(a) for a in range(
            address, address + pic.DESCRIPTOR_BYTES)))
        self.assertGreater(block_end, block_start)
        self.assertEqual(self.crc_in_elf(elf, segments), crc)
        # idempotent
        before = elf.read_bytes()
        rc, out = self.patch(elf, extra=EXCLUDE)
        self.assertEqual((rc, before), (0, elf.read_bytes()), out)
        # what picotool seal does to the block, and what the program does to .data, leaves the CRC alone; a byte of
        # code does not
        self.poke(elf, block_start + 12)
        self.poke(elf, data_start)
        self.assertEqual(self.crc_in_elf(elf, segments), crc)
        self.poke(elf, twice)
        self.assertNotEqual(self.crc_in_elf(elf, segments), crc)

    def test_ram_image_gcc(self):
        self.check_ram_image('gcc')

    def test_ram_image_clang(self):
        self.check_ram_image('clang')

    # lld marks .text writable once it holds an .init_array entry: the code must be covered all the same
    def test_ram_image_with_a_static_constructor_gcc(self):
        self.check_ram_image('gcc', 'RAM_CTOR')

    def test_ram_image_with_a_static_constructor_clang(self):
        self.check_ram_image('clang', 'RAM_CTOR')

    def test_ram_image_refuses_a_flash_segment(self):
        elf = self.build('clang' if shutil.which('ld.lld') else 'gcc', 'KVASIR_IMAGE_CRC=1', 'RAM_IMAGE',
                         'RAM_FLASH_SEGMENT', script='test_ram.ld')
        rc, out = self.patch(elf, SECTIONS + ['.inflash'], EXCLUDE)
        self.assertEqual(rc, 1, out)
        self.assertIn('is not in RAM', out)

    # --sections-from-load-view: the names elf_image.py flashes, so the descriptor covers exactly _flash.hex - the same
    # descriptor as the list, for a flash image (.boot2 .text .data) and a RAM image (every section)
    def test_sections_from_load_view(self):
        toolchain = 'clang' if shutil.which('ld.lld') else 'gcc'
        for defines, script, exclude in ((('KVASIR_IMAGE_CRC=1',), 'test.ld', []),
                                         (('KVASIR_IMAGE_CRC=1', 'RAM_IMAGE'), 'test_ram.ld', EXCLUDE)):
            with self.subTest(script=script):
                view = self.build(toolchain, *defines, script=script)
                listed = Path(self.tmp.name) / f'listed_{view.name}'
                listed.write_bytes(view.read_bytes())
                rc, out = self.patch(listed, extra=exclude)
                self.assertEqual(rc, 0, out)
                rc, out_view = self.patch(view, sections=(), extra=[
                                          '--sections-from-load-view', *exclude])
                self.assertEqual(rc, 0, out_view)
                self.assertEqual(out_view, out)
                self.assertEqual(self.descriptor(
                    view)[1], self.descriptor(listed)[1])

    def test_refuses_an_exclude_the_image_does_not_have(self):
        elf = self.build('clang' if shutil.which('ld.lld')
                         else 'gcc', 'KVASIR_IMAGE_CRC=1')
        rc, out = self.patch(elf, extra=EXCLUDE)
        self.assertEqual(rc, 1, out)
        self.assertIn(
            'no _LINKER_INTERN_after_vectors_start_ in the image', out)

    def test_refuses_an_image_without_descriptor(self):
        elf = self.build('clang' if shutil.which('ld.lld') else 'gcc')
        rc, out = self.patch(elf)
        self.assertEqual(rc, 1, out)
        self.assertIn('no kvasir_image_crc_descriptor', out)

    def test_refuses_a_descriptor_outside_the_hex(self):
        elf = self.build('clang' if shutil.which('ld.lld')
                         else 'gcc', 'KVASIR_IMAGE_CRC=1')
        rc, out = self.patch(elf, ['.boot2', '.data'])
        self.assertEqual(rc, 1, out)
        self.assertIn('not in the flash hex', out)

    def test_refuses_a_ram_segment(self):
        elf = self.build('clang' if shutil.which('ld.lld')
                         else 'gcc', 'KVASIR_IMAGE_CRC=1', 'RAM_SEGMENT')
        rc, out = self.patch(elf, SECTIONS + ['.ramimage'])
        self.assertEqual(rc, 1, out)
        self.assertIn('is in RAM', out)

    def test_refuses_too_many_segments(self):
        elf = self.build('clang' if shutil.which('ld.lld')
                         else 'gcc', 'KVASIR_IMAGE_CRC=1')
        saved = pic.MAX_SEGMENTS
        pic.MAX_SEGMENTS = 1
        try:
            with self.assertRaisesRegex(pic.Refused, 'segments, the descriptor holds 1'):
                pic.patch(elf, self.objcopy, SECTIONS)
        finally:
            pic.MAX_SEGMENTS = saved

    def test_delete_on_failure(self):
        elf = self.build('clang' if shutil.which('ld.lld') else 'gcc')
        result = subprocess.run([sys.executable, '-B', str(PATCH), str(elf), '--objcopy', self.objcopy,
                                 '--delete-on-failure', '--sections', *SECTIONS], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(elf.exists())


class Pieces(unittest.TestCase):
    def test_hex_parser(self):
        def record(kind, offset, data):
            raw = bytes([len(data), offset >> 8, offset & 0xFF, kind]) + data
            return ':' + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper()
        text = '\n'.join([
            record(4, 0, b'\x10\x00'), record(
                0, 0x0000, b'\x01\x02\x03\x04'), record(0, 0x0004, b'\x05\x06'),
            # a gap: a second segment
            record(0, 0x0010, b'\x07'),
            # extended linear address: a third
            record(4, 0, b'\x10\x01'), record(0, 0x0000, b'\x08\x09'),
            # start address: skipped
            record(5, 0, b'\x10\x00\x01\x01'),
            # nothing after the end record
            record(1, 0, b''), record(0, 0, b'\xFF')])
        self.assertEqual(pic.parse_intel_hex(text), [(0x10000000, b'\x01\x02\x03\x04\x05\x06'),
                                                     (0x10000010, b'\x07'), (0x10010000, b'\x08\x09')])
        with self.assertRaises(ValueError):
            pic.parse_intel_hex(':0100000001FF\n')        # checksum

    def test_cut(self):
        segments = [(0x100, bytes(range(32))), (0x200, bytes(8))]
        self.assertEqual(pic.cut(segments, 0x108, 8), [(0x100, bytes(range(8))), (0x110, bytes(range(16, 32))),
                                                       (0x200, bytes(8))])
        self.assertEqual(pic.cut(segments, 0x100, 32), [(0x200, bytes(8))])
        self.assertEqual(pic.cut(segments, 0x118, 8), [
                         (0x100, bytes(range(24))), (0x200, bytes(8))])

    def test_descriptor_bytes(self):
        b = pic.descriptor_bytes(
            0xCAFEF00D, [(0x10000000, b'1234'), (0x10000100, b'12')])
        self.assertEqual(len(b), pic.DESCRIPTOR_BYTES)
        self.assertEqual(struct.unpack('<8I', b[:32]),
                         (pic.MAGIC, 1, 0xCAFEF00D, 2, 0x10000000, 4, 0x10000100, 2))
        self.assertEqual(b[32:], bytes(48))


if __name__ == '__main__':
    unittest.main()
