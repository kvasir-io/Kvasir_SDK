#!/usr/bin/env python3
"""Flash artefacts of a Kvasir image from its load view: what the ELF's program headers say is loaded.

    elf_image.py <elf> --out-prefix <dir>/<target> [--uf2-family 0xE48BFF59] [--objcopy llvm-objcopy]
                 [--eeprom-section .eeprom] [--list]

Selection (elf2bin's "load view", arm-toolchain arm-software/shared/elf2bin): every SHF_ALLOC section with file
contents (not NOBITS, size > 0) that lies inside a PT_LOAD segment with p_filesz > 0 is part of the image, at its load
address (p_paddr + offset in the segment). No section names: a new initialised section is in the image because the
linker loads it. The one name kept is the EEPROM region's section, whose bytes go to separate _eeprom artefacts.

Writes:
  <prefix>_flash.bin  _flash.hex  _flash.uf2  _flash.elf      the image without the EEPROM section
  <prefix>_eeprom.bin _eeprom.hex _eeprom.uf2                 the EEPROM section alone (empty files when none)
  <prefix>_eeprom_flash.hex _eeprom_flash.uf2                 both
The .bin is the image from its lowest to its highest load address, gaps 0x00 (llvm-objcopy -O binary). The .hex
has llvm-objcopy's record layout - 16-byte data records per section from the section's start, an extended segment
(type 02) or linear (type 04) address record when the upper address bits change, the start address record when
e_entry != 0, then EOF. The .uf2 is a 256-byte page grid, uncovered bytes 0x00 (picotool and the RP2 bootrom want
every block page aligned). _flash.elf is `objcopy --only-section` with the selected names and no empty PT_LOAD left
(picotool rejects those). --list prints the selected sections and exits.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

PT_LOAD = 1
SHT_NOBITS = 8
SHF_ALLOC = 0x2


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise ValueError('not a little-endian ELF32 file')
        self.data = data
        (self.entry, phoff, shoff) = struct.unpack_from('<III', data, 24)
        (phentsize, phnum, shentsize, shnum,
         shstrndx) = struct.unpack_from('<HHHHH', data, 42)
        if phnum == 0:
            raise ValueError('no program headers: not a linked image')
        self.segments = [struct.unpack_from(
            '<IIIIIIII', data, phoff + i * phentsize) for i in range(phnum)]
        raw = [struct.unpack_from(
            '<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
        names_off = raw[shstrndx][4] if shnum else 0
        self.sections = []
        for (name, type_, flags, addr, offset, size, *_rest) in raw:
            end = data.index(b'\0', names_off + name)
            self.sections.append({'name': data[names_off + name:end].decode(), 'type': type_, 'flags': flags,
                                  'addr': addr, 'offset': offset, 'size': size})

    def loaded(self):
        """(name, load address, bytes) of every section the image carries, by load address."""
        out = []
        for sec in self.sections:
            if not (sec['flags'] & SHF_ALLOC) or sec['type'] == SHT_NOBITS or sec['size'] == 0:
                continue
            for (p_type, p_offset, _vaddr, p_paddr, p_filesz, *_rest) in self.segments:
                if p_type == PT_LOAD and p_filesz and p_offset <= sec['offset'] \
                        and sec['offset'] + sec['size'] <= p_offset + p_filesz:
                    lma = p_paddr + sec['offset'] - p_offset
                    out.append(
                        (sec['name'], lma, self.data[sec['offset']:sec['offset'] + sec['size']]))
                    break
        return sorted(out, key=lambda piece: piece[1])


def ihex_line(type_, addr, payload):
    body = bytes([len(payload), addr >> 8, addr & 0xFF, type_]) + payload
    return ':' + body.hex().upper() + '%02X' % (-sum(body) & 0xFF) + '\r\n'


def to_ihex(pieces, entry):
    """llvm-objcopy's IHexWriter (llvm/lib/ObjCopy/ELF/ELFObject.cpp, writeSection) for 32-bit addresses."""
    lines = []
    base = 0      # extended linear address (type 04)
    segment = 0   # 8086 segment address (type 02), used below 1 MiB
    for (_name, addr, data) in pieces:
        pos = 0
        while pos < len(data):
            size = min(16, len(data) - pos)
            if addr > segment + base + 0xFFFF:
                if addr > 0xFFFFF:
                    if segment:
                        segment = 0
                        lines.append(ihex_line(2, 0, bytes(2)))
                    base = addr & 0xFFFF0000
                    lines.append(
                        ihex_line(4, 0, struct.pack('>H', base >> 16)))
                else:
                    segment = addr & 0xF0000
                    lines.append(ihex_line(2, 0, bytes([segment >> 12, 0])))
            size = min(size, 0x10000 - (addr - base - segment))
            lines.append(ihex_line(0, addr - base -
                         segment, data[pos:pos + size]))
            addr += size
            pos += size
    if entry > 0xFFFFF:
        lines.append(ihex_line(5, 0, struct.pack('>I', entry)))
    elif entry:   # start segment address (type 03): CS:IP
        lines.append(ihex_line(3, 0, bytes(
            [(entry & 0xF0000) >> 12, 0]) + struct.pack('>H', entry & 0xFFFF)))
    lines.append(ihex_line(1, 0, b''))
    return ''.join(lines).encode()


def to_bin(pieces):
    if not pieces:
        return b''
    start = pieces[0][1]
    out = bytearray(max(addr + len(data)
                    for (_n, addr, data) in pieces) - start)
    for (_n, addr, data) in pieces:
        out[addr - start:addr - start + len(data)] = data
    return bytes(out)


PAGE = 256


def to_uf2(pieces, family):
    pages = {}
    for (_n, addr, data) in pieces:
        for i, byte in enumerate(data):
            page = (addr + i) & ~(PAGE - 1)
            pages.setdefault(page, bytearray(PAGE))[addr + i - page] = byte
    blocks = []
    ordered = sorted(pages.items())
    for number, (page, chunk) in enumerate(ordered):
        head = struct.pack('<IIIIIIII', 0x0A324655, 0x9E5D5157,
                           0x2000, page, PAGE, number, len(ordered), family)
        blocks.append(head + bytes(chunk) + b'\0' *
                      (512 - PAGE - 32 - 4) + struct.pack('<I', 0x0AB16F30))
    return b''.join(blocks)


def strip_empty_segments(path):
    """Drop the PT_LOADs objcopy --only-section left without a section: picotool rejects them."""
    data = bytearray(Path(path).read_bytes())
    (phoff, shoff) = struct.unpack_from('<II', data, 28)
    (phentsize, phnum, shentsize, shnum) = struct.unpack_from('<HHHH', data, 42)
    allocated = []
    for i in range(shnum):
        (_n, type_, flags, addr, _o, size) = struct.unpack_from(
            '<IIIIII', data, shoff + i * shentsize)
        if type_ != 0 and flags & SHF_ALLOC:
            allocated.append((addr, addr + size))
    kept = []
    for i in range(phnum):
        raw = bytes(data[phoff + i * phentsize:phoff + (i + 1) * phentsize])
        (p_type, _off, vaddr, _paddr, _filesz,
         memsz) = struct.unpack_from('<IIIIII', raw)
        if p_type != PT_LOAD or any(start < vaddr + memsz and end > vaddr for (start, end) in allocated):
            kept.append(raw)
    if len(kept) == phnum:
        return
    for i in range(phnum):
        data[phoff + i * phentsize:phoff +
             (i + 1) * phentsize] = kept[i] if i < len(kept) else bytes(phentsize)
    struct.pack_into('<H', data, 44, len(kept))
    Path(path).write_bytes(data)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--out-prefix', type=Path)
    parser.add_argument('--uf2-family', type=lambda s: int(s, 0))
    parser.add_argument('--objcopy')
    parser.add_argument('--eeprom-section', default='.eeprom')
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()

    try:
        elf = Elf(args.elf.read_bytes())
    except ValueError as error:
        print(f'{args.elf}: {error}', file=sys.stderr)
        return 2
    pieces = elf.loaded()
    flash = [p for p in pieces if p[0] != args.eeprom_section]
    eeprom = [p for p in pieces if p[0] == args.eeprom_section]
    if args.list:
        for (name, addr, data) in pieces:
            print(f'{name:20} {addr:#010x} {len(data):#8x}')
        return 0
    if args.out_prefix is None:
        parser.error('--out-prefix is required (or --list)')

    prefix = str(args.out_prefix)
    outputs = {'_flash.bin': to_bin(flash), '_flash.hex': to_ihex(flash, elf.entry),
               '_eeprom.bin': to_bin(eeprom), '_eeprom.hex': to_ihex(eeprom, elf.entry),
               '_eeprom_flash.hex': to_ihex(pieces, elf.entry)}
    if args.uf2_family is not None:
        outputs.update({'_flash.uf2': to_uf2(flash, args.uf2_family), '_eeprom.uf2': to_uf2(eeprom, args.uf2_family),
                        '_eeprom_flash.uf2': to_uf2(pieces, args.uf2_family)})
    for (suffix, content) in outputs.items():
        Path(prefix + suffix).write_bytes(content)
    if args.objcopy:
        only = [f'--only-section={name}' for (name, _a, _d) in flash]
        subprocess.run([args.objcopy, *only, str(args.elf),
                       prefix + '_flash.elf'], check=True)
        strip_empty_segments(prefix + '_flash.elf')
    return 0


if __name__ == '__main__':
    sys.exit(main())
