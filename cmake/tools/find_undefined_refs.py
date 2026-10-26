#!/usr/bin/env python3
"""
Find Undefined References in ELF

    find_undefined_refs.py <elf> [--delete-on-failure]

Lists every undefined symbol and every absolute symbol at 0 of a linked image on stderr, one per line, as
"find_undefined_refs: <elf>: undefined: <name>". FILE symbols, weak undefined ones (they resolve to 0 by design) and
linker/build-system symbols are not counted. The symbol table is read through kvasir_elf (every SHT_SYMTAB entry with a
name: undefined = st_shndx SHN_UNDEF, absolute at 0 = SHN_ABS and st_value 0). --delete-on-failure removes the ELF on exit 1 and 2, as the other post-link checks do, so no person
or tool picks up an image that failed its check.

Exit code: 0 none found, 1 some found, 2 could not check (no such file, not an ELF32 file, no symbol table).
"""

import argparse
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import SHN_ABS, SHN_UNDEF, SHT_SYMTAB, STT_FILE, Elf  # noqa: E402

STB_WEAK = 2


def is_linker_internal_symbol(name):
    """Check if a symbol is a linker-internal or build system symbol that should be ignored."""
    # Patterns for symbols that are expected to be undefined/absolute-zero
    ignore_patterns = [
        r'^ld-temp\.o$',                    # Temporary linker object
        # CMake-generated symbols (cmake_heap_size, etc.)
        r'^cmake_',
        r'^_LINKER_',               # Linker symbols
        r'\.o$',                           # Any object file names
        # ARM EABI C++ exception unwinding personality routines
        r'^__aeabi_unwind_cpp_pr\d+$',
    ]

    for pattern in ignore_patterns:
        if re.search(pattern, name):
            return True

    return False


class CannotCheck(Exception):
    pass


def find_external_references(elf_path):
    """The names of every undefined or absolute-zero symbol that is not a FILE, a weak undefined or a linker one."""
    try:
        elf = Elf.from_path(elf_path)
    except ValueError as e:
        raise CannotCheck(str(e)) from e
    return external_references(elf)


def external_references(elf):
    """find_external_references() on an Elf already read."""
    if not any(s['type'] == SHT_SYMTAB for s in elf.sections):
        raise CannotCheck('no symbol table')
    external_refs = []
    for sym in elf.symbols:
        name = sym['name'].strip()
        # binutils records source file names as FILE symbols in ABS with value 0
        if not name or sym['type'] == STT_FILE:
            continue
        # weak undefined resolves to 0 by design (e.g. newlib-nano's __sf)
        if sym['bind'] == STB_WEAK and sym['shndx'] == SHN_UNDEF:
            continue
        if sym['shndx'] == SHN_UNDEF or (sym['shndx'] == SHN_ABS and sym['value'] == 0):
            if not is_linker_internal_symbol(name):
                external_refs.append(name)
    return external_refs


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf', type=Path)
    ap.add_argument('--delete-on-failure', action='store_true')
    args = ap.parse_args()

    def fail(code):
        if args.delete_on_failure:
            args.elf.unlink(missing_ok=True)
            print(
                f'find_undefined_refs: removed {args.elf.name}', file=sys.stderr)
        return code

    if not args.elf.is_file():
        print(
            f'find_undefined_refs: {args.elf}: no such file', file=sys.stderr)
        return 2
    try:
        elf = Elf.from_path(args.elf)
    except ValueError as e:
        print(f'find_undefined_refs: {args.elf}: {e}', file=sys.stderr)
        return fail(2)
    code = run(elf, args.elf)
    return fail(code) if code else 0


def run(elf, path):
    """The check on an Elf already read from `path`: 0, 1 (undefined symbols, listed) or 2 (no symbol table)."""
    try:
        refs = external_references(elf)
    except CannotCheck as e:
        print(f'find_undefined_refs: {path}: {e}', file=sys.stderr)
        return 2
    for ref in refs:
        print(
            f'find_undefined_refs: {path}: undefined: {ref}', file=sys.stderr)
    return 1 if refs else 0


if __name__ == '__main__':
    sys.exit(main())
