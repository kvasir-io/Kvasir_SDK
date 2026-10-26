#!/usr/bin/env python3
"""Post-link step of IMAGE_CRC: writes the image CRC descriptor (kvasir/Util/ImageDescriptor.hpp) into the ELF.

The covered bytes are the image's load view (kvasir_elf.Elf.loaded(): every section the linker loads, at its load
address) without .eeprom - the pieces elf_image.py writes <name>_flash.hex from, so what J-Link programs and the log
printer checks is what the firmware checks. Pieces that touch form one segment, exactly as uc_log's
printer/HexImage.hpp parseIntelHex joins contiguous records of that hex; the descriptor's own 76 bytes are cut out (its
segment split in two). The CRC is CRC-32/ISO-HDLC chained over the
segments in address order (zlib.crc32(data, previous), the printer's imageCrc).

A RAM image (the descriptor itself linked into SRAM: everything loads into RAM, nothing is copied) is checked where
it runs, so only what must not change while it runs is covered: of the loaded sections, the ones that are not
writable (.vectors, .text with .rodata and, RAM_ONLY, the RAM functions) - .data is left out, it is the program's
own state from the first instruction. The section that holds the descriptor is covered whatever its flags say: the
linker scripts put the descriptor with the constants, and lld marks that section (.text) writable as soon as one
.init_array entry (a static constructor) is in it, though nothing ever writes it. Every segment must then lie in
SRAM; the output names the sections covered and the ones left out.

--exclude START END (symbols, repeatable) cuts [START, END) out as well: bytes a later step rewrites without
changing the program. On the RP2350 that is the picobin block loop in .after_vectors, whose link word picotool seal
and sign change (chip_rp2350/cmake/chip.cmake, kvasir_chip IMAGE_CRC_EXCLUDE).

    patch_image_crc.py <elf> [--exclude START END] [--delete-on-failure]

Runs before every artefact made from the ELF (.bin, .hex, _flash.elf, .uf2, .lst). Idempotent: the descriptor is
not part of what it covers, so a second run writes the same bytes. A signing step (RP2350 picobin hash/signature)
must come after it, and may change only what --exclude names. --delete-on-failure removes the ELF when it fails, so
the next build fails again instead of passing on an unpatched image. Exit code: 0 patched, 1 refused, 2 bad input.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import SHF_ALLOC, SHF_WRITE, SHT_PROGBITS, Elf, merge_contiguous  # noqa: E402

SYMBOL = 'kvasir_image_crc_descriptor'
MAGIC = 0x4B494352
UNPATCHED = 0xFFFFFFFF
MAX_SEGMENTS = 8
DESCRIPTOR_BYTES = 12 + 8 * MAX_SEGMENTS
# the printer compares only below it: RAM changes once the firmware runs
SRAM_BASE = 0x20000000

EEPROM_SECTION = '.eeprom'


class Refused(Exception):
    pass


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
    words = [MAGIC, crc, len(segments)]
    for address, data in segments:
        words += [address, len(data)]
    words += [0] * (2 * (MAX_SEGMENTS - len(segments)))
    return struct.pack(f'<{len(words)}I', *words)


def patch(elf_path, sections=None, excludes=()):
    """Writes the descriptor; `sections` (the names covered) defaults to the load view without .eeprom."""
    try:
        elf = Elf(bytearray(Path(elf_path).read_bytes()))
    except ValueError as e:
        raise ValueError(f'{elf_path}: not a 32-bit little-endian ELF') from e
    result = patch_elf(elf, sections, excludes)
    Path(elf_path).write_bytes(elf.data)
    return result


def patch_elf(elf, sections=None, excludes=()):
    """patch() on an Elf over a bytearray: the descriptor is written into elf.data (the caller writes the file), so
    whatever reads that Elf afterwards sees the patched image."""
    pieces = elf.loaded()
    if sections is None:
        sections = [name for (name, _address, _data)
                    in pieces if name != EEPROM_SECTION]
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

    ram_image = load >= SRAM_BASE
    left_out = []
    if ram_image:
        # the descriptor's own section stays: with an .init_array entry in it lld calls .text writable
        covered = [s['name'] for s in elf.sections if s['name'] in sections and s['type'] == SHT_PROGBITS
                   and s['flags'] & SHF_ALLOC and (s is section or not s['flags'] & SHF_WRITE)]
        present = [s['name']
                   for s in elf.sections if s['name'] in sections and s['size']]
        left_out = [name for name in present if name not in covered]
        sections = covered
    segments = cut(merge_contiguous([(lma, data) for (name, lma, data) in pieces if name in sections]), load,
                   DESCRIPTOR_BYTES)
    for start_symbol, end_symbol in excludes:
        start, end = elf.symbol(start_symbol), elf.symbol(end_symbol)
        if start is None or end is None:
            raise Refused(f'--exclude {start_symbol} {end_symbol}: '
                          f'no {start_symbol if start is None else end_symbol} in the image')
        if end[0] < start[0]:
            raise Refused(
                f'--exclude {start_symbol} {end_symbol}: ends before it starts')
        if end[0] > start[0]:
            first = elf.load_address(start[0])
            if first is None:
                raise Refused(
                    f'--exclude {start_symbol} at 0x{start[0]:08x} is in no loaded segment')
            segments = cut(segments, first, end[0] - start[0])
    if ram_image:
        outside = [a for a, d in segments if a < SRAM_BASE]
        if outside:
            raise Refused(f'a segment at 0x{outside[0]:08x} is not in RAM: a RAM image (the descriptor at '
                          f'0x{address:08x}) is checked where it runs')
    else:
        in_ram = [a for a, d in segments if a + len(d) > SRAM_BASE]
        if in_ram:
            raise Refused(
                f'a segment at 0x{in_ram[0]:08x} is in RAM: the image is not in flash, nor is the descriptor in RAM')
    if len(segments) > MAX_SEGMENTS:
        listing = ', '.join(f'0x{a:08x}+{len(d)}' for a, d in segments)
        raise Refused(f'{len(segments)} segments, the descriptor holds {MAX_SEGMENTS}: {listing} '
                      '(raise Descriptor::maxSegments and MAX_SEGMENTS together)')
    crc = image_crc(segments)
    elf.data[offset:offset +
             DESCRIPTOR_BYTES] = descriptor_bytes(crc, segments)
    return crc, segments, (sections, left_out) if ram_image else None


def main():
    parser = argparse.ArgumentParser(
        description=(__doc__ or '').split('\n')[0])
    parser.add_argument('elf', type=Path)
    parser.add_argument('--exclude', nargs=2, action='append',
                        default=[], metavar=('START', 'END'))
    parser.add_argument('--delete-on-failure', action='store_true')
    args = parser.parse_args()
    try:
        elf = Elf(bytearray(args.elf.read_bytes()))
    except (ValueError, OSError) as e:
        print(f'patch_image_crc: {args.elf.name}: {args.elf}: not a 32-bit little-endian ELF'
              if isinstance(e, ValueError) else f'patch_image_crc: {args.elf.name}: {e}', file=sys.stderr)
        code = 2
    else:
        code = run(elf, args.elf, args.exclude)
    if code and args.delete_on_failure:
        args.elf.unlink(missing_ok=True)
    return code


def run(elf, path, excludes=()):
    """The patch on an Elf over a bytearray read from `path`: the descriptor into elf.data and the file; 0, or 1 (a
    refusal) / 2 (a write that failed) after the message."""
    try:
        crc, segments, ram_image = patch_elf(elf, excludes=excludes)
        Path(path).write_bytes(elf.data)
    except (Refused, ValueError, OSError) as e:
        print(f'patch_image_crc: {Path(path).name}: {e}', file=sys.stderr)
        return 1 if isinstance(e, Refused) else 2
    total = sum(len(d) for _a, d in segments)
    where = ''
    if ram_image:
        covered, left_out = ram_image
        where = f', checked in RAM: {" ".join(covered)}' + (
            f' (left out: {" ".join(left_out)})' if left_out else '')
    print(
        f'image crc: {len(segments)} segment(s), {total} bytes, crc 0x{crc:08x}{where}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
