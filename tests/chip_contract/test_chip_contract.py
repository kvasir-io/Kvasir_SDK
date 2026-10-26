#!/usr/bin/env python3
"""The chip and core contract (cmake/kvasir_chip.cmake) and kvasir_quarantine_interface (cmake/kvasir_interface.cmake):
tests/chip_contract configured once per case. `good` and `empty_lists` must configure (they check the targets, the
properties and the variables themselves), and so must `devices` and `devices_sibling` (the DEVICES key); every other
case must fail and print its message.

    test_chip_contract.py <cmake> [<generator>]
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

PASS = ["good", "empty_lists", "devices", "devices_sibling"]

# (case, text the failing configure prints)
FAIL = [
    ("missing_mpu", "kvasir_chip: MPU is required"),
    ("scratch_size_k", "kvasir_chip: CORE1_SCRATCH_SIZE '4K' is not a size in bytes (decimal)"),
    ("uf2_short", "kvasir_chip: UF2_FAMILY '0xE48BFF' is not 0x and 8 hex digits"),
    ("odd_crc_exclude", "kvasir_chip: IMAGE_CRC_EXCLUDE is pairs of symbols, got 3"),
    ("connect_not_w4", "kvasir_chip: JLINK_CONNECT_COMMANDS 'mww 0x40012004 0x00010000' is not `w4 0x<address>"),
    ("bad_signature", "kvasir_chip: JLINK_PARK_SIGNATURE '0x40064008:0x1' is not <address>,<mask>,<value>"),
    ("definition_with_d",
     "kvasir_chip: DEFINITIONS '-DKVASIR_CHIP_FAKE=1' is not NAME or NAME=value (no -D)"),
    ("linker_file_missing", "does not exist"),
    ("unknown_keyword", "kvasir_chip: unknown argument(s) TARGET_MPU;FAKE"),
    ("devices_with_value", "kvasir_chip: unknown argument(s) ON"),
    ("one_value_empty", "kvasir_chip: no value given for JLINK_FLASH_WINDOW"),
    ("chip_twice", "kvasir_chip: called twice"),
    ("chip_before_core", "kvasir_chip: no kvasir::core"),
    ("registers_not_a_target", "kvasir_chip: REGISTERS peripherals is not a target"),
    ("core_float_abi", "kvasir_core: FLOAT_ABI 'softfloat' is not soft, softfp or hard"),
    ("core_missing_cpu", "kvasir_core: CPU is required"),
]


def configure(cmake, generator, tmp, case):
    cmd = [cmake, "-S", str(HERE), "-B",
           str(Path(tmp) / case), f"-DCASE={case}"]
    if generator:
        cmd += ["-G", generator]
    r = subprocess.run(cmd, capture_output=True, text=True)
    # CMake wraps long messages: compare with the line breaks and indentation folded away
    return r.returncode, " ".join((r.stdout + r.stderr).split())


def main() -> int:
    cmake = sys.argv[1]
    generator = sys.argv[2] if len(sys.argv) > 2 else ""
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for case in PASS:
            rc, out = configure(cmake, generator, tmp, case)
            if rc != 0 or f"chip_contract {case}: ok" not in out:
                failed += 1
                print(f"FAIL {case}: exit {rc}\n{out[-3000:]}")
            else:
                print(f"ok   {case}")
        for case, want in FAIL:
            rc, out = configure(cmake, generator, tmp, case)
            if rc == 0 or " ".join(want.split()) not in out:
                failed += 1
                print(f"FAIL {case}: exit {rc}, want '{want}'\n{out[-3000:]}")
            else:
                print(f"ok   {case}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
