#!/usr/bin/env python3
"""
Strip empty LOAD segments from ELF files.

llvm-objcopy preserves program headers even when sections are removed,
leaving empty LOAD segments that confuse tools like picotool.
This script removes those empty segments.
"""

import struct
import sys
from pathlib import Path


# ELF constants
PT_LOAD = 1
PT_NULL = 0


def read_elf32_header(data):
    """Parse ELF32 header."""
    if data[:4] != b'\x7fELF':
        raise ValueError("Not an ELF file")
    if data[4] != 1:
        raise ValueError("Not a 32-bit ELF")

    header = {}
    header['e_ident'] = data[:16]
    header['e_type'], header['e_machine'], header['e_version'] = struct.unpack_from(
        '<HHI', data, 16)
    header['e_entry'], header['e_phoff'], header['e_shoff'] = struct.unpack_from(
        '<III', data, 24)
    header['e_flags'], header['e_ehsize'] = struct.unpack_from('<IH', data, 36)
    header['e_phentsize'], header['e_phnum'] = struct.unpack_from(
        '<HH', data, 42)
    header['e_shentsize'], header['e_shnum'], header['e_shstrndx'] = struct.unpack_from(
        '<HHH', data, 46)
    return header


def read_program_headers(data, phoff, phentsize, phnum):
    """Parse program headers."""
    phdrs = []
    for i in range(phnum):
        offset = phoff + i * phentsize
        phdr = struct.unpack_from('<IIIIIIII', data, offset)
        phdrs.append({
            'p_type': phdr[0],
            'p_offset': phdr[1],
            'p_vaddr': phdr[2],
            'p_paddr': phdr[3],
            'p_filesz': phdr[4],
            'p_memsz': phdr[5],
            'p_flags': phdr[6],
            'p_align': phdr[7],
            'raw': data[offset:offset + phentsize]
        })
    return phdrs


def read_section_headers(data, shoff, shentsize, shnum):
    """Parse section headers."""
    shdrs = []
    for i in range(shnum):
        offset = shoff + i * shentsize
        shdr = struct.unpack_from('<IIIIIIIIII', data, offset)
        shdrs.append({
            'sh_name': shdr[0],
            'sh_type': shdr[1],
            'sh_flags': shdr[2],
            'sh_addr': shdr[3],
            'sh_offset': shdr[4],
            'sh_size': shdr[5],
            'sh_link': shdr[6],
            'sh_info': shdr[7],
            'sh_addralign': shdr[8],
            'sh_entsize': shdr[9],
        })
    return shdrs


def segment_has_sections(phdr, shdrs):
    """Check if a program header contains any sections."""
    if phdr['p_type'] != PT_LOAD:
        return True  # Keep non-LOAD segments

    seg_start = phdr['p_vaddr']
    seg_end = seg_start + phdr['p_memsz']

    for shdr in shdrs:
        # Skip NULL sections and non-allocated sections
        if shdr['sh_type'] == 0 or (shdr['sh_flags'] & 0x2) == 0:  # SHF_ALLOC = 0x2
            continue

        sec_start = shdr['sh_addr']
        sec_end = sec_start + shdr['sh_size']

        # Check if section overlaps with segment
        if sec_start < seg_end and sec_end > seg_start:
            return True

    return False


def strip_empty_segments(input_path, output_path):
    """Remove empty LOAD segments from ELF file."""
    data = bytearray(Path(input_path).read_bytes())

    header = read_elf32_header(data)
    phdrs = read_program_headers(
        data, header['e_phoff'], header['e_phentsize'], header['e_phnum'])
    shdrs = read_section_headers(
        data, header['e_shoff'], header['e_shentsize'], header['e_shnum'])

    # Find segments to keep
    kept_phdrs = []
    removed_count = 0
    for phdr in phdrs:
        if segment_has_sections(phdr, shdrs):
            kept_phdrs.append(phdr)
        else:
            removed_count += 1
            print(
                f"Removing empty segment at vaddr=0x{phdr['p_vaddr']:08x}", file=sys.stderr)

    if removed_count == 0:
        print("No empty segments found, copying file as-is", file=sys.stderr)
        Path(output_path).write_bytes(data)
        return

    # Rebuild program header table in place
    new_phnum = len(kept_phdrs)
    phoff = header['e_phoff']
    phentsize = header['e_phentsize']

    # Write kept program headers
    for i, phdr in enumerate(kept_phdrs):
        offset = phoff + i * phentsize
        data[offset:offset + phentsize] = phdr['raw']

    # Zero out removed program header slots
    for i in range(new_phnum, header['e_phnum']):
        offset = phoff + i * phentsize
        data[offset:offset + phentsize] = b'\x00' * phentsize

    # Update e_phnum in ELF header (offset 44 for 32-bit ELF)
    struct.pack_into('<H', data, 44, new_phnum)

    Path(output_path).write_bytes(data)
    print(
        f"Removed {removed_count} empty segment(s), kept {new_phnum}", file=sys.stderr)


def main():
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <input.elf> <output.elf>", file=sys.stderr)
        sys.exit(1)

    strip_empty_segments(sys.argv[1], sys.argv[2])


if __name__ == '__main__':
    main()
