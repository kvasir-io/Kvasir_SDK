#!/usr/bin/env python3
"""Post-link check: the reset entry runs nothing the compiler wrote before it has put the core into reset state.

ResetISR (kvasir/StartUp/StartUp.hpp, KVASIR_RESET_ENTRY_ASM) is naked: an image a bootloader or a debugger starts
may come with a stale MSPLIM above SP, CONTROL.SPSEL = 1 or PRIMASK clear, and a compiled prologue's `sub sp` /
`push` is then itself a fault (a stack-pointer update below MSPLIM is the STKOF, Armv8-M ARM DDI0553B.y B3.21).
This reads ResetISR's instructions from the linked ELF and requires:
  1. the first one is `cpsid i`;
  2. up to the `bl` into the C++ body only these: movs rN, #imm; msr MSP/MSPLIM/PSPLIM/CONTROL, rN; isb;
     ldr rN, [pc, #imm] - so nothing writes SP but `msr msp`, and nothing touches the stack;
  3. the `bl` comes within the first 16 instructions.
A compiler, flag or refactor that puts a prologue back fails the link here.

    check_reset_entry.py <elf> [--delete-on-failure]

Exit code: 0 ok, 1 the entry is not as required, 2 bad input (no ResetISR, not an ELF). --delete-on-failure removes the
ELF on 1 and 2.
"""

import argparse
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True      # no __pycache__ in cmake/tools
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kvasir_elf import Elf  # noqa: E402

# MSR <spec_reg>, Rn (T1): 0xF380 | Rn, 0x8800 | SYSm (DDI0553B.y C2.4.126 MSR (register), md l.36970; DDI0419E
# B4.2.3 MSR (register), md l.10103). SYSm: MSP 8, MSPLIM 10, PSPLIM 11, CONTROL 20 (DDI0553B.y C2.4.126, md
# l.37027, l.37029-37030, l.37045; DDI0419E B4.1.1 Table B4-1 for MSP and CONTROL, md l.9957, l.9960).
MSR_ALLOWED = {8: 'msp', 10: 'msplim', 11: 'psplim', 20: 'control'}


def check(code):
    """The list of problems with the entry's instructions (empty when fine)."""
    pos = 0
    count = 0
    while pos + 2 <= len(code) and count < 16:
        (hw,) = struct.unpack_from('<H', code, pos)
        count += 1
        if count == 1:
            if hw != 0xB672:
                return [f'instruction 1 is 0x{hw:04x}, not cpsid i (0xb672): something runs before PRIMASK is set']
            pos += 2
            continue
        if (hw & 0xF800) in (0xE800, 0xF000, 0xF800):   # 32-bit Thumb-2 encoding
            (hw2,) = struct.unpack_from('<H', code, pos + 2)
            if (hw & 0xF800) == 0xF000 and (hw2 & 0xD000) == 0xD000:
                return []   # bl: the C++ body
            if (hw & 0xFFF0) == 0xF380 and (hw2 & 0xFF00) == 0x8800:
                sysm = hw2 & 0xFF
                if sysm not in MSR_ALLOWED:
                    return [f'instruction {count}: msr to SYSm {sysm}, not one of {sorted(MSR_ALLOWED.values())}']
                pos += 4
                continue
            if hw == 0xF3BF and (hw2 & 0xFFF0) == 0x8F60:
                pos += 4   # isb
                continue
            return [f'instruction {count}: 0x{hw:04x} 0x{hw2:04x} is not allowed before the bl into the body']
        if (hw & 0xF800) == 0x2000 or (hw & 0xF800) == 0x4800:
            pos += 2   # movs rN, #imm8 / ldr rN, [pc, #imm8]
            continue
        return [f'instruction {count}: 0x{hw:04x} is not allowed before the bl into the body '
                '(a compiled prologue? the stack must not be touched here)']
    return ['no bl into the body within the first 16 instructions']


def run(elf, path):
    """The check on an Elf already read from `path`: 0, 1 (a problem) or 2 (no ResetISR to check), messages printed."""
    found = elf.symbol('ResetISR')
    if found is None:
        print(f'{path}: no symbol ResetISR', file=sys.stderr)
        return 2
    entry = found[0]
    code = elf.bytes_at(entry & ~1, 64)
    if code is None:
        print(
            f'{path}: ResetISR at 0x{entry:08x} lies in no loaded section', file=sys.stderr)
        return 2
    problems = check(code)
    if not problems:
        return 0
    for p in problems:
        print(f'{path}: ResetISR at 0x{entry & ~1:08x}: {p}', file=sys.stderr)
    return 1


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('elf')
    parser.add_argument('--delete-on-failure', action='store_true')
    args = parser.parse_args()
    path = Path(args.elf)
    try:
        code = run(Elf.from_path(path), path)
    except ValueError:
        print(f'{path}: not a 32-bit little-endian ELF', file=sys.stderr)
        code = 2
    if code and args.delete_on_failure:
        path.unlink(missing_ok=True)
    return code


if __name__ == '__main__':
    sys.exit(main())
