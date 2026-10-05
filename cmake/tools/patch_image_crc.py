#!/usr/bin/env python3
"""Post-link step of IMAGE_CRC: writes the image CRC descriptor (kvasir/Util/ImageDescriptor.hpp) into the ELF.

The covered bytes are the data records of the flash hex - the same `objcopy -O ihex --only-section=...` that
generate_object makes <name>_flash.hex with, so what J-Link programs and the log printer checks is what the firmware
checks. Contiguous records form one segment, exactly as uc_log's detail/HexImage.hpp parseIntelHex builds them; the
descriptor's own 80 bytes are cut out (its segment split in two). The CRC is CRC-32/ISO-HDLC chained over the
segments in address order (zlib.crc32(data, previous), the printer's imageCrc).

    patch_image_crc.py <elf> --objcopy <llvm-objcopy> --sections .vectors .text .data ... [--delete-on-failure]

Runs before every artefact made from the ELF (.bin, .hex, _flash.elf, .uf2, .lst). Idempotent: the descriptor is
not part of what it covers, so a second run writes the same bytes. A signing step (RP2350 picobin hash/signature)
must come after it. --delete-on-failure removes the ELF when it fails, so the next build fails again instead of
passing on an unpatched image. Exit code: 0 patched, 1 refused, 2 bad input.
"""

import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

SYMBOL = 'kvasir_image_crc_descriptor'
MAGIC = 0x4B494352
UNPATCHED = 0xFFFFFFFF
VERSION = 1
MAX_SEGMENTS = 8
DESCRIPTOR_BYTES = 16 + 8 * MAX_SEGMENTS
# the printer compares only below it: RAM changes once the firmware runs
SRAM_BASE = 0x20000000

SHT_SYMTAB = 2


class Refused(Exception):
    pass


class Elf:
    """Sections, program headers and symbols of an ELF32 little-endian file."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise ValueError(f'{path}: not a 32-bit little-endian ELF')
        self.data = bytearray(data)
        phoff, shoff = struct.unpack_from('<II', data, 0x1C)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from(
            '<HHHHH', data, 0x2A)
        raw = [struct.unpack_from(
            '<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
        names = raw[shstrndx][4]
        self.sections = []
        for (name, kind, flags, addr, offset, size, link, _info, _align, entsize) in raw:
            self.sections.append({'name': self._str(names, name), 'type': kind, 'flags': flags, 'addr': addr,
                                  'offset': offset, 'size': size, 'link': link, 'entsize': entsize})
        self.segments = [struct.unpack_from(
            '<IIIIIIII', data, phoff + i * phentsize) for i in range(phnum)]

    def _str(self, table_offset, index):
        end = self.data.index(0, table_offset + index)
        return self.data[table_offset + index:end].decode()

    def symbol(self, wanted):
        for s in self.sections:
            if s['type'] != SHT_SYMTAB:
                continue
            strtab = self.sections[s['link']]['offset']
            for i in range(s['size'] // s['entsize']):
                name, value, size, _info, _other, _shndx = struct.unpack_from(
                    '<IIIBBH', self.data, s['offset'] + i * s['entsize'])
                if name != 0 and self._str(strtab, name) == wanted:
                    return value, size
        return None

    def section_of(self, address):
        for s in self.sections:
            if s['flags'] & 0x2 and s['addr'] <= address < s['addr'] + s['size']:
                return s
        return None

    def load_address(self, address):
        for (kind, _offset, vaddr, paddr, filesz, _memsz, _flags, _align) in self.segments:
            if kind == 1 and vaddr <= address < vaddr + filesz:
                return paddr + (address - vaddr)
        return None


def parse_intel_hex(text):
    """[(address, bytes)]: contiguous data records joined, as uc_log's parseIntelHex does."""
    segments = []
    base = 0
    for number, line in enumerate(text.splitlines(), 1):
        line = line.rstrip('\r ')
        if not line:
            continue
        if line[0] != ':' or len(line) < 11 or len(line) % 2 == 0:
            raise ValueError(f'hex line {number}: not a record')
        raw = bytes.fromhex(line[1:])
        if sum(raw) & 0xFF:
            raise ValueError(f'hex line {number}: checksum')
        count, offset, kind = raw[0], (raw[1] << 8) | raw[2], raw[3]
        if len(raw) != count + 5:
            raise ValueError(f'hex line {number}: length')
        if kind == 1:
            break
        if kind == 2 and count == 2:
            base = ((raw[4] << 8) | raw[5]) << 4
        elif kind == 4 and count == 2:
            base = ((raw[4] << 8) | raw[5]) << 16
        elif kind == 0:
            address = base + offset
            if not segments or segments[-1][0] + len(segments[-1][1]) != address:
                segments.append((address, bytearray()))
            segments[-1][1].extend(raw[4:4 + count])
    return [(a, bytes(d)) for a, d in segments]


def cut(segments, start, length):
    """The segments without [start, start + length)."""
    out = []
    end = start + length
    for address, data in segments:
        seg_end = address + len(data)
        if seg_end <= start or address >= end:
            out.append((address, data))
            continue
        if address < start:
            out.append((address, data[:start - address]))
        if seg_end > end:
            out.append((end, data[end - address:]))
    return out


def image_crc(segments):
    crc = 0
    for _address, data in segments:
        crc = zlib.crc32(data, crc)
    return crc


def descriptor_bytes(crc, segments):
    words = [MAGIC, VERSION, crc, len(segments)]
    for address, data in segments:
        words += [address, len(data)]
    words += [0] * (2 * (MAX_SEGMENTS - len(segments)))
    return struct.pack(f'<{len(words)}I', *words)


def flash_hex(elf_path, objcopy, sections):
    with tempfile.TemporaryDirectory() as tmp:
        hex_path = Path(tmp) / 'image.hex'
        cmd = [objcopy, '--output-target', 'ihex'] + [f'--only-section={s}' for s in sections] + \
              [str(elf_path), str(hex_path)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise ValueError(f'{" ".join(cmd)}: {result.stderr.strip()}')
        return hex_path.read_text()


def patch(elf_path, objcopy, sections):
    elf = Elf(elf_path)
    found = elf.symbol(SYMBOL)
    if found is None:
        raise Refused(f'no {SYMBOL} in the image: IMAGE_CRC is on, but no translation unit of it includes '
                      'kvasir/StartUp/StartUp.hpp (or kvasir/Util/ImageDescriptor.hpp)')
    address, size = found
    if size not in (0, DESCRIPTOR_BYTES):
        raise Refused(
            f'{SYMBOL} is {size} bytes, not {DESCRIPTOR_BYTES}: this script and ImageDescriptor.hpp differ')
    section = elf.section_of(address)
    if section is None or section['name'] not in sections:
        where = section['name'] if section else 'no section'
        raise Refused(f'{SYMBOL} is in {where}, which is not in the flash hex ({" ".join(sections)}): the linker '
                      'script does not place .kvasir_image_crc through linker/common_text_body.inc.ld')
    offset = section['offset'] + (address - section['addr'])
    (magic,) = struct.unpack_from('<I', elf.data, offset)
    if magic not in (UNPATCHED, MAGIC):
        raise Refused(
            f'{SYMBOL} at 0x{address:08x} starts with 0x{magic:08x}: not a descriptor')
    load = elf.load_address(address)
    if load is None:
        raise Refused(f'{SYMBOL} at 0x{address:08x} is in no loaded segment')

    segments = cut(parse_intel_hex(
        flash_hex(elf_path, objcopy, sections)), load, DESCRIPTOR_BYTES)
    in_ram = [a for a, d in segments if a + len(d) > SRAM_BASE]
    if in_ram:
        raise Refused(
            f'a segment at 0x{in_ram[0]:08x} is in RAM: the image is not in flash (RAM_ONLY?)')
    if len(segments) > MAX_SEGMENTS:
        listing = ', '.join(f'0x{a:08x}+{len(d)}' for a, d in segments)
        raise Refused(f'{len(segments)} segments, the descriptor holds {MAX_SEGMENTS}: {listing} '
                      '(raise Descriptor::MaxSegments and MAX_SEGMENTS together)')
    crc = image_crc(segments)
    elf.data[offset:offset +
             DESCRIPTOR_BYTES] = descriptor_bytes(crc, segments)
    Path(elf_path).write_bytes(elf.data)
    return crc, segments


def main():
    parser = argparse.ArgumentParser(
        description=(__doc__ or '').split('\n')[0])
    parser.add_argument('elf', type=Path)
    parser.add_argument('--objcopy', required=True)
    parser.add_argument('--sections', nargs='+', required=True)
    parser.add_argument('--delete-on-failure', action='store_true')
    args = parser.parse_args()
    try:
        crc, segments = patch(args.elf, args.objcopy, args.sections)
    except (Refused, ValueError, OSError) as e:
        print(f'patch_image_crc: {args.elf.name}: {e}', file=sys.stderr)
        if args.delete_on_failure:
            args.elf.unlink(missing_ok=True)
        return 1 if isinstance(e, Refused) else 2
    total = sum(len(d) for _a, d in segments)
    print(
        f'image crc: {len(segments)} segment(s), {total} bytes, crc 0x{crc:08x}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
