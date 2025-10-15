#!/usr/bin/env python3
"""
Find Undefined References in ELF
Outputs undefined symbols and absolute zero symbols, one per line.
Exit code: 0 if none found, 1 if any found.
"""

import subprocess
import sys
import re
from pathlib import Path


def is_linker_internal_symbol(name):
    """Check if a symbol is a linker-internal or build system symbol that should be ignored."""
    # Patterns for symbols that are expected to be undefined/absolute-zero
    ignore_patterns = [
        r'^ld-temp\.o$',                    # Temporary linker object
        # CMake-generated symbols (cmake_heap_size, etc.)
        r'^cmake_',
        r'^_LINKER_INTERN_',               # Linker internal symbols
        r'\.o$',                           # Any object file names
        # ARM EABI C++ exception unwinding personality routines
        r'^__aeabi_unwind_cpp_pr\d+$',
    ]

    for pattern in ignore_patterns:
        if re.search(pattern, name):
            return True

    return False


def find_external_references(elf_path):
    """Find all external/undefined references using readelf."""
    result = subprocess.run(
        ['readelf', '-s', '-W', str(elf_path)],
        capture_output=True,
        text=True
    )

    external_refs = []
    for line in result.stdout.split('\n'):
        # Match symbol table entries
        match = re.search(
            r'^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(.+)$', line)
        if match:
            value, size, stype, bind, vis, ndx, name = match.groups()
            name = name.strip()

            # Check for undefined (UND section) or absolute with value 0
            if ndx == 'UND' or (ndx == 'ABS' and value == '00000000'):
                # Filter out linker-internal symbols
                if not is_linker_internal_symbol(name):
                    external_refs.append(name)

    return external_refs


def main():
    if len(sys.argv) < 2:
        print("Usage: python find_undefined_refs.py <elf_file>", file=sys.stderr)
        sys.exit(1)

    elf_file = Path(sys.argv[1])

    if not elf_file.exists():
        print(f"Error: File not found: {elf_file}", file=sys.stderr)
        sys.exit(1)

    try:
        refs = find_external_references(elf_file)

        for ref in refs:
            print(ref)

        # Exit with 1 if any found, 0 otherwise
        sys.exit(1 if refs else 0)

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
