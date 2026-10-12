#!/usr/bin/env python3
"""cmake/tools/check_reset_entry.py's instruction rules, on hand-assembled Thumb code (no toolchain needed)."""

import struct
import sys
from pathlib import Path

sys.path.insert(
    0, str(Path(__file__).resolve().parents[2] / 'cmake' / 'tools'))
import check_reset_entry  # noqa: E402


def code(*halfwords):
    return struct.pack('<' + 'H' * len(halfwords), *halfwords) + bytes(64)


CPSID = 0xB672
MOVS_R0_0 = 0x2000
MSR = {'msplim': (0xF380, 0x880A), 'psplim': (0xF380, 0x880B), 'control': (0xF380, 0x8814), 'msp': (0xF380, 0x8808),
       'psp': (0xF380, 0x8809), 'primask': (0xF380, 0x8810)}
ISB = (0xF3BF, 0x8F6F)
LDR_R0_PC = 0x4802
BL = (0xF000, 0xF800)
SUB_SP = 0xB086
PUSH = 0xB580

cases = [
    ('the M33 entry', code(CPSID, MOVS_R0_0, *MSR['msplim'], *MSR['psplim'], *MSR['control'], *ISB, LDR_R0_PC,
                           *MSR['msp'], *BL), True),
    ('the M0+ entry', code(CPSID, MOVS_R0_0, *
     MSR['control'], *ISB, LDR_R0_PC, *MSR['msp'], *BL), True),
    ('a prologue first', code(SUB_SP, CPSID, MOVS_R0_0, *BL), False),
    ('a push before the bl', code(CPSID, PUSH, *BL), False),
    ('sub sp before the bl', code(CPSID, MOVS_R0_0, SUB_SP, *BL), False),
    ('msr psp', code(CPSID, MOVS_R0_0, *MSR['psp'], *BL), False),
    ('msr primask', code(CPSID, MOVS_R0_0, *MSR['primask'], *BL), False),
    ('no bl', code(CPSID, *([MOVS_R0_0] * 20)), False),
]

failed = 0
for name, blob, ok in cases:
    problems = check_reset_entry.check(blob)
    if (not problems) != ok:
        print(
            f'FAIL {name}: expected {"ok" if ok else "a problem"}, got {problems}')
        failed += 1
    else:
        print(f'ok   {name}{": " + problems[0] if problems else ""}')
sys.exit(1 if failed else 0)
