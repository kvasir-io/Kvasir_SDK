#!/usr/bin/env python3
"""The one ELF32 little-endian reader of the post-link tools (and of tools/kvasir_bench.py): sections, program headers,
symbols, bytes by address and the image's load view.

    sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from kvasir_elf import Elf

The lookups by address are NOT interchangeable; each keeps the semantics of the tool it came from:
  read(addr, n)         the SHF_ALLOC section (size > 0) holding addr by VMA; None in NOBITS or past its end
  bytes_at(addr, n)     the first SHF_ALLOC, non-NOBITS section holding the whole range
  progbits_at(addr, n)  the first SHT_PROGBITS section holding the whole range, ALLOC or not (.vectors is PROGBITS,
                        an INIT_ARRAY section is not)
  section_of(addr)      the first SHF_ALLOC section holding addr by VMA, NOBITS included
  load_address(addr)    addr's load address through the PT_LOAD holding it (p_filesz part)
The load view (elf2bin's, arm-toolchain arm-software/shared/elf2bin): every SHF_ALLOC section with contents inside a
PT_LOAD with p_filesz > 0, at p_paddr + its offset in the segment.

A file that is not little-endian ELF32 raises ValueError('not a little-endian ELF32 file') at construction; each tool
catches it and prints its own text. A file without program headers (a relocatable object) reads fine; only the load
view (loaded, load_addresses, resident) raises ValueError on it.
"""

import struct
from pathlib import Path

PT_LOAD = 1
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
STT_FUNC = 2
STT_FILE = 4


class Elf:
    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise ValueError('not a little-endian ELF32 file')
        # kept as given: patch_image_crc passes a bytearray and writes it back
        self.data = data
        (self.entry, phoff, shoff) = struct.unpack_from('<III', data, 24)
        (phentsize, phnum, shentsize, shnum,
         shstrndx) = struct.unpack_from('<HHHHH', data, 42)
        self.segments = [struct.unpack_from(
            '<IIIIIIII', data, phoff + i * phentsize) for i in range(phnum)]
        raw = [struct.unpack_from(
            '<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
        names = raw[shstrndx][4] if shnum else 0
        self.sections = []
        for (name, type_, flags, addr, offset, size, link, info, align, entsize) in raw:
            self.sections.append({'name': self._str(names, name), 'type': type_, 'flags': flags, 'addr': addr,
                                  'offset': offset, 'size': size, 'link': link, 'info': info, 'align': align,
                                  'entsize': entsize})
        # every entry of every SHT_SYMTAB, the null symbol included
        self.symbols = []
        for sec in self.sections:
            if sec['type'] != SHT_SYMTAB or not sec['entsize']:
                continue
            strtab = self.sections[sec['link']]['offset']
            for pos in range(sec['offset'], sec['offset'] + sec['size'] - sec['entsize'] + 1, sec['entsize']):
                (name, value, size, info, _other,
                 shndx) = struct.unpack_from('<IIIBBH', data, pos)
                self.symbols.append({'name': self._str(strtab, name), 'value': value, 'size': size,
                                     'type': info & 0xF, 'bind': info >> 4, 'shndx': shndx})

    @classmethod
    def from_path(cls, path):
        return cls(Path(path).read_bytes())

    def _str(self, table_offset, index):
        end = self.data.index(0, table_offset + index)
        return bytes(self.data[table_offset + index:end]).decode('utf-8', 'replace')

    # --- symbols and sections by name ---

    def symbol(self, name):
        """(value, size) of the first symbol of that name, or None."""
        return next(((s['value'], s['size']) for s in self.symbols if s['name'] and s['name'] == name), None)

    def section(self, name):
        return next((s for s in self.sections if s['name'] == name), None)

    # --- by address ---

    def alloc_sections(self):
        """The SHF_ALLOC sections with a size, NOBITS included."""
        return [s for s in self.sections if s['flags'] & SHF_ALLOC and s['size']]

    def alloc_section_at(self, address):
        return next((s for s in self.alloc_sections() if s['addr'] <= address < s['addr'] + s['size']), None)

    def read(self, address, size):
        sec = self.alloc_section_at(address)
        if sec is None or sec['type'] == SHT_NOBITS or address + size > sec['addr'] + sec['size']:
            return None
        start = sec['offset'] + address - sec['addr']
        return self.data[start:start + size]

    def bytes_at(self, address, count):
        for s in self.sections:
            if s['flags'] & SHF_ALLOC and s['type'] != SHT_NOBITS and s['addr'] <= address \
                    and address + count <= s['addr'] + s['size']:
                start = s['offset'] + address - s['addr']
                return self.data[start:start + count]
        return None

    def progbits_at(self, address, count):
        for s in self.sections:
            if s['type'] == SHT_PROGBITS and s['addr'] <= address and address + count <= s['addr'] + s['size']:
                start = s['offset'] + address - s['addr']
                return self.data[start:start + count]
        return None

    def section_of(self, address):
        return next((s for s in self.sections if s['flags'] & SHF_ALLOC
                     and s['addr'] <= address < s['addr'] + s['size']), None)

    def load_address(self, address):
        for (kind, _offset, vaddr, paddr, filesz, _memsz, _flags, _align) in self.segments:
            if kind == PT_LOAD and vaddr <= address < vaddr + filesz:
                return paddr + (address - vaddr)
        return None

    # --- the load view ---

    def _loaded(self):
        """(section, load address, bytes) of every section the image carries, by load address."""
        if not self.segments:
            raise ValueError('no program headers: not a linked image')
        out = []
        for sec in self.sections:
            if not (sec['flags'] & SHF_ALLOC) or sec['type'] == SHT_NOBITS or sec['size'] == 0:
                continue
            for (p_type, p_offset, _vaddr, p_paddr, p_filesz, *_rest) in self.segments:
                if p_type == PT_LOAD and p_filesz and p_offset <= sec['offset'] \
                        and sec['offset'] + sec['size'] <= p_offset + p_filesz:
                    lma = p_paddr + sec['offset'] - p_offset
                    out.append(
                        (sec, lma, self.data[sec['offset']:sec['offset'] + sec['size']]))
                    break
        return sorted(out, key=lambda piece: piece[1])

    def loaded(self):
        """(name, load address, bytes) of every section the image carries, by load address."""
        return [(sec['name'], lma, data) for (sec, lma, data) in self._loaded()]

    def load_addresses(self):
        """{name: load address} of every section the image carries."""
        return {sec['name']: lma for (sec, lma, _data) in self._loaded()}

    def resident(self):
        """(name, address, bytes) of every loaded section that runs where it is loaded (address == load address)
        and is not writable (SHF_WRITE clear): what a running target holds there is exactly these bytes. A section
        copied elsewhere at boot (.data, code run from RAM) or one the program may write is left out, whatever its
        address."""
        return [(sec['name'], lma, data) for (sec, lma, data) in self._loaded()
                if sec['addr'] == lma and not sec['flags'] & SHF_WRITE]


def merge_contiguous(pieces):
    """[(address, bytes)] sorted by address, every piece whose start is the previous one's end joined to it."""
    out = []
    for (address, data) in sorted(pieces, key=lambda p: p[0]):
        if out and out[-1][0] + len(out[-1][1]) == address:
            out[-1][1].extend(data)
        else:
            out.append((address, bytearray(data)))
    return [(a, bytes(d)) for (a, d) in out]
