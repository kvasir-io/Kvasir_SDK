#!/usr/bin/env python3
"""Flash artefacts of a Kvasir image from its load view: what the ELF's program headers say is loaded.

    elf_image.py <elf> --out-prefix <dir>/<target> [--uf2-family 0xE48BFF59] [--objcopy llvm-objcopy]
                 [--eeprom-section .eeprom] [--list] [--delete-on-failure]

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
every block page aligned). _flash.elf is `objcopy --only-section` with the selected names, no empty PT_LOAD left
(picotool rejects those) and no PT_LOAD with a hole between two sections larger than the second one's alignment
(picotool seal rejects those). --list prints the selected sections and exits.

The image end check: `_LINKER_image_end_flash_` (Kvasir_SDK linker/common.ld, the end of the flash image that Kvasir
code reads) is wrong when a selected section other than the EEPROM one ends past it in flash - its load address below
`_LINKER_INTERN_rom_end_` (common_flash.ld): the chip's linker script loads a section after .data without moving
`_LINKER_INTERN_image_end_flash_`. Exit code 1, nothing written. A RAM-only image has the symbol 0 and is not checked.
Exit code 2: not a linked ELF32 file, or one without the symbols (its linker script does not include common.ld).
--delete-on-failure removes the ELF on exit 1 and 2, as the other post-link checks do.
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import PT_LOAD, SHF_ALLOC, SHT_NOBITS, Elf  # noqa: E402

IMAGE_END = '_LINKER_image_end_flash_'
ROM_END = '_LINKER_INTERN_rom_end_'


def required_symbol(elf, name):
    found = elf.symbol(name)
    if found is None:
        raise ValueError(
            f'no {name} symbol: the linker script does not include Kvasir_SDK linker/common.ld')
    return found[0]


def past_image_end(elf, pieces):
    """The messages for every piece (name, load address, bytes) that ends past _LINKER_image_end_flash_ in flash; none
    when the symbol is 0 (RAM only). ValueError when the image lacks the symbols."""
    limit = required_symbol(elf, IMAGE_END)
    if not limit:
        return []
    rom_end = required_symbol(elf, ROM_END)
    out = []
    for (name, addr, data) in pieces:
        end = addr + len(data)
        if end > limit and addr < rom_end:
            out.append(f'section {name} is loaded at {addr:#010x}..{end:#010x}, {end - limit} bytes past {IMAGE_END} '
                       f'({limit:#010x}): the linker script loads it into flash after the image end; define '
                       f'_LINKER_INTERN_image_end_flash_ past it before INCLUDE common.ld')
    return out


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


def strip_empty_segments(data, elf):
    """Drop the PT_LOADs objcopy --only-section left without a section: picotool rejects them. `elf` reads `data` (a
    bytearray), both changed in place: elf.segments is the kept list afterwards."""
    (phoff,) = struct.unpack_from('<I', data, 28)
    (phentsize, phnum) = struct.unpack_from('<HH', data, 42)
    allocated = [(s['addr'], s['addr'] + s['size'])
                 for s in elf.sections if s['type'] != 0 and s['flags'] & SHF_ALLOC]
    kept = []
    for (i, segment) in enumerate(elf.segments):
        (p_type, _off, vaddr, _paddr, _filesz, memsz, *_rest) = segment
        if p_type != PT_LOAD or any(start < vaddr + memsz and end > vaddr for (start, end) in allocated):
            kept.append(
                bytes(data[phoff + i * phentsize:phoff + (i + 1) * phentsize]))
    if len(kept) == phnum:
        return
    for i in range(phnum):
        data[phoff + i * phentsize:phoff +
             (i + 1) * phentsize] = kept[i] if i < len(kept) else bytes(phentsize)
    struct.pack_into('<H', data, 44, len(kept))
    elf.segments = [struct.unpack('<IIIIIIII', header) for header in kept]


def split_segments_at_holes(data, elf):
    """Split a PT_LOAD where picotool seal cannot plug the hole between two of its sections: one larger than the next
    section's alignment ("Cannot plug gap greater than alignment"). GNU ld puts a RAM-only image's .vectors, .text
    and .data into one segment, with the NOLOAD .noInitLowRam (between .vectors and .text) and .ramExec (between .text
    and .data, 256 bytes) as holes in it (common_ram_only.ld); lld writes a segment per section already. A hole up to
    the alignment stays in its segment, as picotool would plug it. Every byte stays at its address and a split-off
    hole stays unloaded; the last part keeps the segment's zero-filled tail (p_memsz past p_filesz). `data` (a bytearray)
    is changed in place, `elf` (reading it) is not."""
    (phoff, shoff) = struct.unpack_from('<II', data, 28)
    phnum = len(elf.segments)
    sections = []
    used = [shoff] if elf.sections else []
    for s in elf.sections:
        if s['type'] not in (0, SHT_NOBITS) and s['size']:
            used.append(s['offset'])
            if s['flags'] & SHF_ALLOC:
                sections.append((s['offset'], s['size'], s['align']))
    sections.sort()
    segments = []
    for segment in elf.segments:
        (p_type, p_offset, vaddr, paddr, filesz, memsz, p_flags, p_align) = segment
        inside = [s for s in sections if p_offset <= s[0]
                  and s[0] + s[1] <= p_offset + filesz]
        if p_type != PT_LOAD or not inside:
            segments.append(segment)
            continue
        parts = [[inside[0][0], inside[0][0] + inside[0][1]]]
        for (offset, size, align) in inside[1:]:
            if offset - parts[-1][1] > align:
                parts.append([offset, offset + size])
            else:
                parts[-1][1] = offset + size
        for (n, (start, end)) in enumerate(parts):
            delta = start - p_offset
            segments.append((PT_LOAD, start, vaddr + delta, paddr + delta, end - start,
                             memsz - delta if n == len(parts) - 1 else end - start, p_flags, p_align))
    if len(segments) == phnum:
        return
    table = b''.join(struct.pack('<IIIIIIII', *s) for s in segments)
    # the longer table in place: a linked image keeps its first bytes on a page boundary, kilobytes past the headers
    room = min(t for t in used + [len(data)] if t > phoff) - phoff
    if len(table) > room:
        raise ValueError(
            f'{len(segments)} program headers do not fit into the {room} bytes before the next contents')
    data[phoff:phoff + len(table)] = table
    struct.pack_into('<H', data, 44, len(segments))


def fix_flash_elf(path):
    """objcopy's _flash.elf made acceptable to picotool: read and parsed once, both fixes on that buffer, written once
    when one of them changed it."""
    data = bytearray(Path(path).read_bytes())
    original = bytes(data)
    elf = Elf(data)
    strip_empty_segments(data, elf)
    try:
        split_segments_at_holes(data, elf)
    except ValueError as error:
        raise ValueError(f'{path}: {error}') from error
    if data != original:
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
    parser.add_argument('--delete-on-failure', action='store_true')
    args = parser.parse_args()

    def malformed(error):
        print(f'{args.elf}: {error}', file=sys.stderr)
        if args.delete_on_failure:
            args.elf.unlink(missing_ok=True)
        return 2

    try:
        elf = Elf(args.elf.read_bytes())
        pieces = elf.loaded()
    except ValueError as error:
        return malformed(error)
    if args.list:
        for (name, addr, data) in pieces:
            print(f'{name:20} {addr:#010x} {len(data):#8x}')
        return 0
    if args.out_prefix is None:
        parser.error('--out-prefix is required (or --list)')
    code = run(elf, args.elf, args.out_prefix, args.uf2_family,
               args.objcopy, args.eeprom_section)
    if code and args.delete_on_failure:
        args.elf.unlink(missing_ok=True)
    return code


def run(elf, path, out_prefix, uf2_family=None, objcopy=None, eeprom_section='.eeprom'):
    """The artefacts of an Elf already read from `path` (objcopy reads the file itself: it must hold elf's bytes): 0,
    1 (past the image end) or 2 (not a linked image), messages printed."""
    try:
        pieces = elf.loaded()
        flash = [p for p in pieces if p[0] != eeprom_section]
        eeprom = [p for p in pieces if p[0] == eeprom_section]
        problems = past_image_end(elf, flash)
    except ValueError as error:
        print(f'{path}: {error}', file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f'{path}: error: {problem}', file=sys.stderr)
        return 1

    prefix = str(out_prefix)
    outputs = {'_flash.bin': to_bin(flash), '_flash.hex': to_ihex(flash, elf.entry),
               '_eeprom.bin': to_bin(eeprom), '_eeprom.hex': to_ihex(eeprom, elf.entry),
               '_eeprom_flash.hex': to_ihex(pieces, elf.entry)}
    if uf2_family is not None:
        outputs.update({'_flash.uf2': to_uf2(flash, uf2_family), '_eeprom.uf2': to_uf2(eeprom, uf2_family),
                        '_eeprom_flash.uf2': to_uf2(pieces, uf2_family)})
    for (suffix, content) in outputs.items():
        Path(prefix + suffix).write_bytes(content)
    if objcopy:
        only = [f'--only-section={name}' for (name, _a, _d) in flash]
        subprocess.run([objcopy, *only, str(path),
                       prefix + '_flash.elf'], check=True)
        fix_flash_elf(prefix + '_flash.elf')
    return 0


if __name__ == '__main__':
    sys.exit(main())
