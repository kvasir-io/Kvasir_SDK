#!/usr/bin/env python3
"""Post-link check: every RAM function really runs from RAM, and so does everything it reaches.

A function that must run while the flash is not readable (XIP off, a flash erase, the QMI being
reconfigured) is written with KVASIR_RAM_FUNC_ATTRIBUTES and starts with KVASIR_RAM_FUNC_MARK().
The mark is a label inside the function whose address the assembler writes into the non-loaded
section `kvasir_ram_funcs`: it travels with the code, so it names the place the compiler really
put the function, whatever became of the section attribute (gcc with LTO drops it for member,
template and inline functions unless they are also noipa).

For every mark this script checks, in the linked ELF:
  1. the function that contains it lies entirely in a writable (RAM) section;
  2. nothing it can reach runs from a read-only loaded section (flash): every direct branch,
     call and tail call, every function address loaded from a literal pool (gcc's long_call:
     ldr rN, =f; blx rN) or built with movw/movt (lld's long-branch thunks), followed through
     every RAM function reached that way. Calls into ROM (outside every section) are allowed.
Indirect calls through a register loaded from anywhere else cannot be followed.

    check_ram_funcs.py <elf> [--delete-on-failure] [--all-ram-functions] [--cxxfilt TOOL]

--delete-on-failure removes the ELF when the check fails, so a failed POST_BUILD step is not
forgotten by the next build. --all-ram-functions also checks every function found in RAM without
a mark (a survey; RAM code that runs from RAM only for speed may call flash). Exit code: 0 ok,
1 a RAM function is not contained, 2 bad input.
"""

import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

MARK_SECTION = 'kvasir_ram_funcs'

SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
STT_FUNC = 2


class Elf:
    """The little of an ELF32 little-endian file this needs: sections, symbols, bytes."""

    def __init__(self, path):
        data = Path(path).read_bytes()
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise ValueError(f'{path}: not a 32-bit little-endian ELF')
        self.data = data
        (shoff,) = struct.unpack_from('<I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
        raw = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize)
               for i in range(shnum)]
        names = raw[shstrndx]
        self.sections = []
        for (name, stype, flags, addr, offset, size, link, _info, _align, entsize) in raw:
            self.sections.append({'name': self._str(names[4], name), 'type': stype, 'flags': flags,
                                  'addr': addr, 'offset': offset, 'size': size, 'link': link,
                                  'entsize': entsize})
        self.symbols = []
        for sec in self.sections:
            if sec['type'] != SHT_SYMTAB:
                continue
            strtab = self.sections[sec['link']]['offset']
            for i in range(sec['size'] // 16):
                name, value, size, info, _other, shndx = struct.unpack_from(
                    '<IIIBBH', data, sec['offset'] + i * 16)
                self.symbols.append({'name': self._str(strtab, name), 'value': value,
                                     'size': size, 'type': info & 0xF, 'shndx': shndx})

    def _str(self, table_offset, index):
        end = self.data.index(b'\0', table_offset + index)
        return self.data[table_offset + index:end].decode('utf-8', 'replace')

    def section(self, name):
        return next((s for s in self.sections if s['name'] == name), None)

    def loaded(self):
        return [s for s in self.sections if s['flags'] & SHF_ALLOC and s['size']]

    def section_at(self, address):
        return next((s for s in self.loaded() if s['addr'] <= address < s['addr'] + s['size']), None)

    def read(self, address, size):
        sec = self.section_at(address)
        if sec is None or sec['type'] == SHT_NOBITS or address + size > sec['addr'] + sec['size']:
            return None
        start = sec['offset'] + address - sec['addr']
        return self.data[start:start + size]


class Regions:
    """Where RAM and flash are: the linker's _LINKER_INTERN_{ram,rom}_{start,end}_ (the memory
    regions of Kvasir_SDK linker/common*.ld), else - an image linked by another script - the
    section flags (writable = RAM, loaded read-only = flash). A RAM-only image has no flash."""

    def __init__(self, elf):
        symbols = {s['name']: s['value'] for s in elf.symbols}
        self.elf = elf

        def region(name):
            start = symbols.get(f'_LINKER_INTERN_{name}_start_')
            end = symbols.get(f'_LINKER_INTERN_{name}_end_')
            return (start, end) if start is not None and end is not None else None
        self.ram, self.flash = region('ram'), region('rom')
        self.from_symbols = self.ram is not None

    def is_ram(self, address):
        if self.from_symbols:
            return self.ram[0] <= address < self.ram[1]
        sec = self.elf.section_at(address)
        return sec is not None and bool(sec['flags'] & SHF_WRITE)

    def is_flash(self, address):
        if self.from_symbols:
            return self.flash is not None and self.flash[0] <= address < self.flash[1]
        sec = self.elf.section_at(address)
        return sec is not None and sec['type'] == SHT_PROGBITS and not sec['flags'] & SHF_WRITE


def sign_extend(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Image:
    def __init__(self, elf):
        self.elf = elf
        self.functions = sorted((s for s in elf.symbols if s['type'] == STT_FUNC and s['size']),
                                key=lambda s: s['value'])
        self.function_starts = {s['value'] & ~1: s for s in self.functions}
        # ARM mapping symbols: $t / $t.N starts Thumb code, $d / $d.N data (literal pools)
        self.mapping = sorted((s['value'], s['name'][1]) for s in elf.symbols
                              if s['name'][:2] in ('$t', '$d', '$a')
                              and (len(s['name']) == 2 or s['name'][2] == '.'))

    def function_at(self, address):
        for f in self.functions:
            start = f['value'] & ~1
            if start <= address < start + f['size']:
                return f
        return None

    def data_ranges(self, start, end):
        """[(from, to)] inside [start, end) that the mapping symbols mark as data."""
        ranges, kind, since = [], 't', start
        for address, k in self.mapping:
            if address <= start:
                kind = k
                continue
            if address >= end:
                break
            if kind == 'd':
                ranges.append((since, address))
            kind, since = k, address
        if kind == 'd':
            ranges.append((since, end))
        return ranges

    def references(self, function):
        """(instruction address, target, how) of every code address the function can go to."""
        start = function['value'] & ~1
        end = start + function['size']
        code = self.elf.read(start, function['size'])
        if code is None:
            return
        data = self.data_ranges(start, end)
        movw = {}
        pc = start
        while pc + 2 <= end:
            skip = next((to for (fr, to) in data if fr <= pc < to), None)
            if skip is not None:
                pc = skip
                continue
            hw1 = struct.unpack_from('<H', code, pc - start)[0]
            if hw1 >> 11 in (0b11101, 0b11110, 0b11111) and pc + 4 <= end:
                hw2 = struct.unpack_from('<H', code, pc - start + 2)[0]
                yield from self._wide(pc, hw1, hw2, movw)
                pc += 4
                continue
            yield from self._narrow(pc, hw1)
            pc += 2

    def _literal(self, address):
        word = self.elf.read(address, 4)
        return None if word is None else struct.unpack('<I', word)[0]

    def _code_pointer(self, pc, value, how):
        # a Thumb function address: odd, and the start of a function symbol
        if value is not None and value & 1 and (value & ~1) in self.function_starts:
            yield pc, value & ~1, how

    def _narrow(self, pc, hw1):
        if hw1 >> 11 == 0b11100:                                   # B T2
            yield pc, pc + 4 + sign_extend((hw1 & 0x7FF) << 1, 12), 'b'
        elif hw1 >> 12 == 0b1101 and (hw1 >> 8) & 0xF < 0xE:       # B<c> T1
            yield pc, pc + 4 + sign_extend((hw1 & 0xFF) << 1, 9), 'b<c>'
        # LDR Rt, [PC, #imm] T1
        elif hw1 >> 11 == 0b01001:
            address = ((pc + 4) & ~3) + (hw1 & 0xFF) * 4
            yield from self._code_pointer(pc, self._literal(address), 'ldr =function')
        elif hw1 & 0xF500 == 0xB100:                               # CB(N)Z
            yield pc, pc + 4 + (((hw1 >> 9) & 1) << 6 | ((hw1 >> 3) & 0x1F) << 1), 'cbz'

    def _wide(self, pc, hw1, hw2, movw):
        if hw1 >> 11 == 0b11110 and hw2 >> 14 & 0b10:
            s, j1, j2 = (hw1 >> 10) & 1, (hw2 >> 13) & 1, (hw2 >> 11) & 1
            if hw2 & 0x1000:                                       # BL T1 / B.W T4
                i1, i2 = ~(j1 ^ s) & 1, ~(j2 ^ s) & 1
                offset = (s << 24 | i1 << 23 | i2 << 22 | (
                    hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1)
                yield pc, pc + 4 + sign_extend(offset, 25), 'bl' if hw2 & 0x4000 else 'b.w'
            elif not hw2 & 0x4000 and (hw1 >> 6) & 0xF < 0xE:     # B<c>.W T3
                offset = (s << 20 | j2 << 19 | j1 << 18 | (
                    hw1 & 0x3F) << 12 | (hw2 & 0x7FF) << 1)
                yield pc, pc + 4 + sign_extend(offset, 21), 'b<c>.w'
        # LDR.W Rt, [PC, #+-imm12]
        elif hw1 & 0xFF7F == 0xF85F:
            imm = hw2 & 0xFFF
            base = (pc + 4) & ~3
            yield from self._code_pointer(pc, self._literal(base + imm if hw1 & 0x80 else base - imm),
                                          'ldr.w =function')
        elif hw1 & 0xFBF0 in (0xF240, 0xF2C0):                     # MOVW / MOVT
            imm16 = ((hw1 & 0xF) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8
                     | (hw2 & 0xFF))
            rd = (hw2 >> 8) & 0xF
            if hw1 & 0xFBF0 == 0xF240:
                movw[rd] = imm16
            elif rd in movw:
                yield from self._code_pointer(pc, imm16 << 16 | movw.pop(rd), 'movw/movt =function')


def demangler(tool):
    exe = tool or next(
        (t for t in ('llvm-cxxfilt', 'c++filt') if shutil.which(t)), None)
    cache = {}

    def demangle(name):
        if exe is None or not name.startswith('_Z'):
            return name
        if name not in cache:
            # gcc's clone suffixes: .constprop.0, .isra.0
            base, _, suffix = name.partition('.')
            out = subprocess.run(
                [exe, base], capture_output=True, text=True).stdout.strip()
            cache[name] = (out or base) + (f' [{suffix}]' if suffix else '')
        return cache[name]
    return demangle


def marks(elf, section):
    sec = elf.section(section)
    if sec is None:
        return []
    body = elf.data[sec['offset']:sec['offset'] + sec['size']]
    # 0 / all ones: the linker's tombstone for a mark whose function was garbage-collected
    return [a for (a,) in struct.iter_unpack('<I', body[:len(body) // 4 * 4]) if a not in (0, 0xFFFFFFFF)]


def check(elf, all_ram_functions, name):
    image = Image(elf)
    regions = Regions(elf)
    problems = []
    roots = []   # (function, walk what it reaches, marked)
    for section, walk in ((MARK_SECTION, True), (MARK_SECTION + '_calls_flash', False)):
        for address in marks(elf, section):
            function = image.function_at(address)
            if function is None:
                sec = elf.section_at(address)
                problems.append(f'a RAM function mark at {address:#010x} '
                                f'({sec["name"] if sec else "no section"}) is inside no function symbol')
                continue
            roots.append((function, walk, True))
    if all_ram_functions:
        seen = {id(f) for f, _, _ in roots}
        roots += [(f, True, False) for f in image.functions
                  if regions.is_ram(f['value'] & ~1) and id(f) not in seen]

    checked = set()
    for root, walk, is_marked in roots:
        start = root['value'] & ~1
        outside = next(
            (a for a in (start, start + root['size'] - 1) if not regions.is_ram(a)), None)
        if outside is not None:
            sec = elf.section_at(outside)
            problems.append(f'{name(root["name"])} is at {start:#010x} in '
                            f'{sec["name"] if sec else "no section"}, not in RAM'
                            + ('' if is_marked else ' (unmarked)'))
            continue
        if not walk:
            continue
        path = {start: [root]}   # how the walk got to each function
        todo = [root]
        while todo:
            function = todo.pop()
            fstart = function['value'] & ~1
            if fstart in checked:
                continue
            checked.add(fstart)
            for pc, target, how in image.references(function):
                if fstart <= target < fstart + function['size']:
                    continue
                callee = image.function_at(target)
                if regions.is_flash(target):
                    sec = elf.section_at(target)
                    chain = ' -> '.join(name(f['name']) for f in path[fstart])
                    what = name(
                        callee['name']) if callee else f'{target:#010x}'
                    problems.append(f'{chain} reaches flash: {how} at {pc:#010x} -> {what} '
                                    f'({sec["name"] if sec else "flash"} {target:#010x})'
                                    + ('' if is_marked else ' (unmarked RAM function)'))
                elif regions.is_ram(target) and callee is not None:
                    cstart = callee['value'] & ~1
                    if cstart not in path:
                        path[cstart] = path[fstart] + [callee]
                        todo.append(callee)
    return problems, sum(1 for r in roots if r[2]), len(checked)


# ---- every RAM function in the image carries its mark ---------------------------------------
# The ELF check only sees functions that have a mark; this one sees the sources. Which sources:
# the DWARF line tables' file list, i.e. exactly the files that put code into this image.

ATTRIBUTE = re.compile(r'\bKVASIR_RAM_FUNC_ATTRIBUTES\b')
MARK = re.compile(r'\s*KVASIR_RAM_FUNC_MARK(_CALLS_FLASH)?\s*\(\s*\)')


def strip_comments(text):
    """Comments and string literals blanked out, line breaks kept (so offsets map to lines)."""
    def blank(m):
        return ''.join(c if c == '\n' else ' ' for c in m.group(0))
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                  blank, text, flags=re.S)


def unmarked_definitions(path):
    """(line, text) of every KVASIR_RAM_FUNC_ATTRIBUTES function definition in the file whose body
    does not begin with a mark. A declaration (';' before '{') needs none."""
    try:
        text = strip_comments(Path(path).read_text(errors='replace'))
    except OSError:
        return []
    found = []
    for m in ATTRIBUTE.finditer(text):
        line_start = text.rfind('\n', 0, m.start()) + 1
        if text[line_start:m.start()].lstrip().startswith('#'):
            continue                                   # the #define itself
        # past the attribute list [[...]]
        close = text.find(']]', m.end())
        depth, i = 0, (close + 2 if close != -1 else m.end())
        while i < len(text):
            c = text[i]
            if c in '([':
                depth += 1
            elif c in ')]':
                depth -= 1
            elif depth == 0 and c == ';':
                break
            elif depth == 0 and c == '{':
                if not MARK.match(text, i + 1):
                    line = text.count('\n', 0, m.start()) + 1
                    head = re.sub(r'\[\[.*?\]\]', ' ',
                                  text[m.start():i], flags=re.S)
                    found.append(
                        (line, (head.split('(')[0].split() or ['?'])[-1]))
                break
            i += 1
    return found


def image_sources(elf_path, tool):
    exe = tool or shutil.which('llvm-dwarfdump')
    if exe is None:
        return None
    out = subprocess.run(
        [exe, '--show-sources', str(elf_path)], capture_output=True, text=True)
    return sorted({line.strip() for line in out.stdout.splitlines() if line.strip()})


def lint(elf_path, tool):
    sources = image_sources(elf_path, tool)
    if sources is None:
        return None, ['llvm-dwarfdump not found: the sources were not checked for unmarked RAM functions']
    problems = []
    for source in sources:
        for line, what in unmarked_definitions(source):
            problems.append(f'{source}:{line}: {what}: a KVASIR_RAM_FUNC_ATTRIBUTES function without '
                            'KVASIR_RAM_FUNC_MARK() as its first statement')
    return problems, []


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('elf', type=Path)
    ap.add_argument('--delete-on-failure', action='store_true')
    ap.add_argument('--all-ram-functions', action='store_true')
    ap.add_argument('--no-sources', action='store_true',
                    help='skip the unmarked-function check')
    ap.add_argument('--cxxfilt')
    ap.add_argument('--dwarfdump')
    args = ap.parse_args()
    try:
        elf = Elf(args.elf)
    except (OSError, ValueError) as e:
        print(f'check_ram_funcs: {e}', file=sys.stderr)
        return 2
    problems, marked, reached = check(
        elf, args.all_ram_functions, demangler(args.cxxfilt))
    notes = []
    if not args.no_sources:
        unmarked, notes = lint(args.elf, args.dwarfdump)
        problems += unmarked or []
    for n in notes:
        print(f'check_ram_funcs: note: {n}', file=sys.stderr)
    if not problems:
        return 0
    print(f'check_ram_funcs: {args.elf.name}: {len(problems)} problem(s) with the {marked} marked RAM '
          f'function(s) ({reached} function(s) reached):', file=sys.stderr)
    for p in problems:
        print(f'  {p}', file=sys.stderr)
    if args.delete_on_failure:
        args.elf.unlink(missing_ok=True)
        print(f'check_ram_funcs: removed {args.elf.name}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
